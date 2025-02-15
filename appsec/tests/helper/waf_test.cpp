// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2021 Datadog, Inc.
#include "common.hpp"
#include "engine_settings.hpp"
#include "json_helper.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <regex>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <subscriber/waf.hpp>
#include <tags.hpp>
#include <utils.hpp>

const std::string waf_rule =
    R"({"version": "2.1", "metadata": {"rules_version": "1.2.3"}, "rules": [{"id": "1", "name": "rule1", "tags": {"type": "flow1", "category": "category1"}, "conditions": [{"operator": "match_regex", "parameters": {"inputs": [{"address": "arg1", "key_path": [] } ], "regex": "^string.*"} }, {"operator": "match_regex", "parameters": {"inputs": [{"address": "arg2", "key_path": [] } ], "regex": ".*"} } ], "action": "record"} ], "processors": [{"id": "processor-001", "generator": "extract_schema", "parameters": {"mappings": [{"inputs": [{"address": "arg2"} ], "output": "_dd.appsec.s.arg2"} ], "scanners": [{"tags": {"category": "pii"} } ] }, "evaluate": false, "output": true } ], "scanners": [] })";
const std::string waf_rule_with_data =
    R"({"version":"2.1","rules":[{"id":"blk-001-001","name":"Block IP Addresses","tags":{"type":"block_ip","category":"security_response"},"conditions":[{"parameters":{"inputs":[{"address":"http.client_ip"}],"data":"blocked_ips"},"operator":"ip_match"}],"transformers":[],"on_match":["block"]}]})";

namespace dds {

template <typename Mutex>
class log_counter_sink : public spdlog::sinks::base_sink<Mutex> {
public:
    size_t count() const noexcept { return counter; }
    void clear() noexcept { counter = 0; }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override { counter++; }

    void flush_() override {}

    size_t counter{0};
};

using log_counter_sink_st = log_counter_sink<spdlog::details::null_mutex>;

TEST(WafTest, InitWithInvalidRules)
{
    engine_settings cs;
    cs.rules_file = create_sample_rules_invalid();
    auto ruleset = engine_ruleset::from_path(cs.rules_file);
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_settings(cs, ruleset, meta, metrics)};

    EXPECT_EQ(meta.size(), 2);
    EXPECT_STREQ(meta[std::string(tag::waf_version)].c_str(), "1.20.1");

    rapidjson::Document doc;
    doc.Parse(meta[std::string(tag::event_rules_errors)]);
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_TRUE(doc.IsObject());
    EXPECT_TRUE(doc.HasMember("missing key 'type'"));
    EXPECT_TRUE(doc.HasMember("unknown matcher: squash"));
    EXPECT_TRUE(doc.HasMember("missing key 'inputs'"));

    EXPECT_EQ(metrics.size(), 2);
    // For small enough integers this comparison should work, otherwise replace
    // with EXPECT_NEAR.
    EXPECT_EQ(metrics[tag::event_rules_loaded], 1.0);
    EXPECT_EQ(metrics[tag::event_rules_failed], 4.0);
}

TEST(WafTest, RunWithInvalidParam)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule, meta, metrics)};
    auto ctx = wi->get_listener();
    parameter_view pv;
    dds::event e;
    EXPECT_THROW(ctx->call(pv, e), invalid_object);
}

TEST(WafTest, RunWithTimeout)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi(
        waf::instance::from_string(waf_rule, meta, metrics, 0));
    auto ctx = wi->get_listener();

    auto p = parameter::map();
    p.add("arg1", parameter::string("string 1"sv));
    p.add("arg2", parameter::string("string 2"sv));

    parameter_view pv(p);
    dds::event e;
    EXPECT_THROW(ctx->call(pv, e), timeout_error);
}

TEST(WafTest, ValidRunGood)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule, meta, metrics)};
    auto ctx = wi->get_listener();

    auto p = parameter::map();
    p.add("arg1", parameter::string("string 1"sv));

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_STREQ(meta[std::string(tag::event_rules_version)].c_str(), "1.2.3");
    EXPECT_GT(metrics[tag::waf_duration], 0.0);
}

TEST(WafTest, ValidRunMonitor)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule, meta, metrics)};
    auto ctx = wi->get_listener();

    auto p = parameter::map();
    p.add("arg1", parameter::string("string 1"sv));
    p.add("arg2", parameter::string("string 3"sv));

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    for (auto &match : e.data) {
        rapidjson::Document doc;
        doc.Parse(match);
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_TRUE(doc.IsObject());
    }

    EXPECT_TRUE(e.actions.empty());
    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_STREQ(meta[std::string(tag::event_rules_version)].c_str(), "1.2.3");
    EXPECT_GT(metrics[tag::waf_duration], 0.0);
}

TEST(WafTest, ValidRunMonitorObfuscated)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule, meta, metrics,
            waf::instance::default_waf_timeout_us, "password"sv, "string 3"sv)};
    auto ctx = wi->get_listener();

    auto p = parameter::map(), sub_p = parameter::map();
    sub_p.add("password", parameter::string("string 1"sv));
    p.add("arg1", std::move(sub_p));
    p.add("arg2", parameter::string("string 3"sv));

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    EXPECT_EQ(e.data.size(), 1);
    rapidjson::Document doc;
    doc.Parse(e.data[0]);
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_TRUE(doc.IsObject());

    EXPECT_STREQ(doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
        "<Redacted>");
    EXPECT_STREQ(doc["rule_matches"][1]["parameters"][0]["value"].GetString(),
        "<Redacted>");

    EXPECT_TRUE(e.actions.empty());

    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_STREQ(meta[std::string(tag::event_rules_version)].c_str(), "1.2.3");
    EXPECT_GT(metrics[tag::waf_duration], 0.0);
}

TEST(WafTest, ValidRunMonitorObfuscatedFromSettings)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    engine_settings cs;
    cs.rules_file = create_sample_rules_ok();
    cs.obfuscator_key_regex = "password";
    auto ruleset = engine_ruleset::from_path(cs.rules_file);

    std::shared_ptr<subscriber> wi{
        waf::instance::from_settings(cs, ruleset, meta, metrics)};

    auto ctx = wi->get_listener();

    auto p = parameter::map(), sub_p = parameter::map();
    sub_p.add("password", parameter::string("acunetix-product"sv));
    p.add("server.request.headers.no_cookies", std::move(sub_p));

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    EXPECT_EQ(e.data.size(), 1);
    rapidjson::Document doc;
    doc.Parse(e.data[0]);
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_TRUE(doc.IsObject());

    EXPECT_TRUE(e.actions.empty());

    EXPECT_STREQ(doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
        "<Redacted>");

    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_STREQ(meta[std::string(tag::event_rules_version)].c_str(), "1.2.3");
    EXPECT_GT(metrics[tag::waf_duration], 0.0);
}

TEST(WafTest, UpdateRuleData)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule_with_data, meta, metrics)};
    ASSERT_TRUE(wi);

    auto addresses = wi->get_subscriptions();
    EXPECT_EQ(addresses.size(), 1);
    EXPECT_STREQ(addresses.begin()->c_str(), "http.client_ip");

    {
        auto ctx = wi->get_listener();

        auto p = parameter::map();
        p.add("http.client_ip", parameter::string("192.168.1.1"sv));

        parameter_view pv(p);
        dds::event e;
        ctx->call(pv, e);
    }

    auto param = json_to_parameter(
        R"({"rules_data":[{"id":"blocked_ips","type":"data_with_expiration","data":[{"value":"192.168.1.1","expiration":"9999999999"}]}]})");

    wi = wi->update(param, meta, metrics);
    ASSERT_TRUE(wi);

    addresses = wi->get_subscriptions();
    EXPECT_EQ(addresses.size(), 1);
    EXPECT_STREQ(addresses.begin()->c_str(), "http.client_ip");

    {
        auto ctx = wi->get_listener();

        auto p = parameter::map();
        p.add("http.client_ip", parameter::string("192.168.1.1"sv));

        parameter_view pv(p);
        dds::event e;
        ctx->call(pv, e);

        EXPECT_EQ(e.data.size(), 1);
        rapidjson::Document doc;
        doc.Parse(e.data[0]);
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_TRUE(doc.IsObject());

        EXPECT_STREQ(
            doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
            "192.168.1.1");

        EXPECT_EQ(e.actions.size(), 1);
        EXPECT_EQ(e.actions.begin()->type, dds::action_type::block);
    }
}

TEST(WafTest, UpdateInvalid)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule_with_data, meta, metrics)};
    ASSERT_TRUE(wi);

    {
        auto ctx = wi->get_listener();

        auto p = parameter::map();
        p.add("http.client_ip", parameter::string("192.168.1.1"sv));

        parameter_view pv(p);
        dds::event e;
        ctx->call(pv, e);
    }

    auto param = json_to_parameter(R"({})");

    ASSERT_THROW(wi->update(param, meta, metrics), invalid_object);
}

TEST(WafTest, SchemasAreAdded)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    std::shared_ptr<subscriber> wi{
        waf::instance::from_string(waf_rule, meta, metrics)};
    auto ctx = wi->get_listener();

    auto p = parameter::map(), sub_p = parameter::map();
    sub_p.add("password", parameter::string("string 1"sv));
    p.add("arg1", std::move(sub_p));
    p.add("arg2", parameter::string("string 3"sv));

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    EXPECT_EQ(e.data.size(), 1);
    rapidjson::Document doc;
    doc.Parse(e.data[0]);
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_TRUE(doc.IsObject());

    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_FALSE(meta.empty());
    EXPECT_STREQ(meta["_dd.appsec.s.arg2"].c_str(), "[8]");
}

TEST(WafTest, FingerprintAreNotAdded)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    engine_settings settings;
    settings.rules_file = create_sample_rules_ok();
    auto ruleset = engine_ruleset::from_path(settings.rules_file);

    std::shared_ptr<subscriber> wi{
        waf::instance::from_settings(settings, ruleset, meta, metrics)};
    auto ctx = wi->get_listener();

    auto p = parameter::map();

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_FALSE(meta.empty());
    EXPECT_STREQ(meta["_dd.appsec.fp.http.endpoint"].c_str(), "");
    EXPECT_STREQ(meta["_dd.appsec.fp.http.network"].c_str(), "");
    EXPECT_STREQ(meta["_dd.appsec.fp.http.header"].c_str(), "");
    EXPECT_STREQ(meta["_dd.appsec.fp.fp.session"].c_str(), "");
}

TEST(WafTest, FingerprintAreAdded)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    engine_settings settings;
    settings.rules_file = create_sample_rules_ok();
    auto ruleset = engine_ruleset::from_path(settings.rules_file);

    std::shared_ptr<subscriber> wi{
        waf::instance::from_settings(settings, ruleset, meta, metrics)};
    auto ctx = wi->get_listener();

    auto p = parameter::map();

    // Endpoint Fingerprint inputs
    auto query = parameter::map();
    query.add("query", parameter::string("asdfds"sv));
    p.add("server.request.uri.raw", parameter::string("asdfds"sv));
    p.add("server.request.method", parameter::string("GET"sv));
    p.add("server.request.query", std::move(query));

    // Network and Headers Fingerprint inputs
    auto headers = parameter::map();
    headers.add("X-Forwarded-For", parameter::string("192.168.72.0"sv));
    headers.add("user-agent", parameter::string("acunetix-product"sv));
    p.add("server.request.headers.no_cookies", std::move(headers));

    // Session Fingerprint inputs
    p.add("server.request.cookies", parameter::string("asdfds"sv));
    p.add("usr.session_id", parameter::string("asdfds"sv));
    p.add("usr.id", parameter::string("asdfds"sv));

    parameter_view pv(p);
    dds::event e;
    ctx->call(pv, e);

    ctx->get_meta_and_metrics(meta, metrics);
    EXPECT_FALSE(meta.empty());
    EXPECT_THAT(meta["_dd.appsec.fp.http.endpoint"].c_str(),
        MatchesRegex("http-get(-[A-Za-z0-9]*){3}"));

    EXPECT_THAT(meta["_dd.appsec.fp.http.network"].c_str(),
        MatchesRegex("net-[0-9]*-[a-zA-Z0-9]*"));

    EXPECT_THAT(meta["_dd.appsec.fp.http.header"].c_str(),
        MatchesRegex("hdr(-[0-9]*-[a-zA-Z0-9]*){2}"));

    EXPECT_THAT(meta["_dd.appsec.fp.session"].c_str(),
        MatchesRegex("ssn(-[a-zA-Z0-9]*){4}"));
}

TEST(WafTest, ActionsAreSentAndParsed)
{
    std::map<std::string, std::string> meta;
    std::map<std::string_view, double> metrics;

    auto p = parameter::map();
    p.add("http.client_ip", parameter::string("192.168.1.1"sv));
    parameter_view pv(p);

    { // Standard actions types with custom parameters
        std::string rules_with_actions =
            R"({"version":"2.1","rules":[{"id":"blk-001-001","name":"BlockIPAddresses","tags":{"type":"block_ip","category":"security_response"},"conditions":[{"parameters":{"inputs":[{"address":"http.client_ip"}],"data":"blocked_ips"},"operator":"ip_match"}],"transformers":[],"on_match":["custom"]}],"actions":[{"id":"custom","type":"block_request","parameters":{"status_code":123,"grpc_status_code":321,"type":"json","custom_param":"foo"}}],"rules_data":[{"id":"blocked_ips","type":"data_with_expiration","data":[{"value":"192.168.1.1","expiration":"9999999999"}]}]})";

        std::shared_ptr<subscriber> wi{
            waf::instance::from_string(rules_with_actions, meta, metrics)};
        ASSERT_TRUE(wi);

        auto addresses = wi->get_subscriptions();
        EXPECT_EQ(addresses.size(), 1);
        EXPECT_STREQ(addresses.begin()->c_str(), "http.client_ip");

        auto ctx = wi->get_listener();

        dds::event e;
        ctx->call(pv, e);

        EXPECT_EQ(e.data.size(), 1);
        rapidjson::Document doc;
        doc.Parse(e.data[0]);
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_TRUE(doc.IsObject());

        EXPECT_STREQ(
            doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
            "192.168.1.1");

        auto action = e.actions.begin();
        EXPECT_EQ(e.actions.size(), 1);
        EXPECT_EQ(action->type, dds::action_type::block);
        EXPECT_STREQ(
            action->parameters.find("status_code")->second.c_str(), "123");
        EXPECT_STREQ(
            action->parameters.find("grpc_status_code")->second.c_str(), "321");
        EXPECT_STREQ(action->parameters.find("type")->second.c_str(), "json");
        EXPECT_STREQ(
            action->parameters.find("custom_param")->second.c_str(), "foo");
    }

    { // Standard actions types with no parameters
        std::string rules_with_actions =
            R"({"version":"2.1","rules":[{"id":"blk-001-001","name":"BlockIPAddresses","tags":{"type":"block_ip","category":"security_response"},"conditions":[{"parameters":{"inputs":[{"address":"http.client_ip"}],"data":"blocked_ips"},"operator":"ip_match"}],"transformers":[],"on_match":["custom"]}],"actions":[{"id":"custom","type":"block_request","parameters":{}}],"rules_data":[{"id":"blocked_ips","type":"data_with_expiration","data":[{"value":"192.168.1.1","expiration":"9999999999"}]}]})";

        std::shared_ptr<subscriber> wi{
            waf::instance::from_string(rules_with_actions, meta, metrics)};
        ASSERT_TRUE(wi);

        auto addresses = wi->get_subscriptions();
        EXPECT_EQ(addresses.size(), 1);
        EXPECT_STREQ(addresses.begin()->c_str(), "http.client_ip");

        auto ctx = wi->get_listener();

        dds::event e;
        ctx->call(pv, e);

        EXPECT_EQ(e.data.size(), 1);
        rapidjson::Document doc;
        doc.Parse(e.data[0]);
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_TRUE(doc.IsObject());

        EXPECT_STREQ(
            doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
            "192.168.1.1");

        auto action = e.actions.begin();
        EXPECT_EQ(e.actions.size(), 1);
        EXPECT_EQ(action->type, dds::action_type::block);
        EXPECT_STREQ(action->parameters.find("status_code")->second.c_str(),
            "403"); // Default value
        EXPECT_STREQ(
            action->parameters.find("grpc_status_code")->second.c_str(),
            "10"); // Default value
        EXPECT_STREQ(action->parameters.find("type")->second.c_str(),
            "auto"); // Default value
    }

    { // Custom action types
        std::string rules_with_actions =
            R"({"version":"2.1","rules":[{"id":"blk-001-001","name":"BlockIPAddresses","tags":{"type":"block_ip","category":"security_response"},"conditions":[{"parameters":{"inputs":[{"address":"http.client_ip"}],"data":"blocked_ips"},"operator":"ip_match"}],"transformers":[],"on_match":["custom"]}],"actions":[{"id":"custom","type":"custom_type","parameters":{"some":"parameter"}}],"rules_data":[{"id":"blocked_ips","type":"data_with_expiration","data":[{"value":"192.168.1.1","expiration":"9999999999"}]}]})";

        std::shared_ptr<subscriber> wi{
            waf::instance::from_string(rules_with_actions, meta, metrics)};
        ASSERT_TRUE(wi);

        auto addresses = wi->get_subscriptions();
        EXPECT_EQ(addresses.size(), 1);
        EXPECT_STREQ(addresses.begin()->c_str(), "http.client_ip");

        auto ctx = wi->get_listener();

        dds::event e;
        ctx->call(pv, e);

        EXPECT_EQ(e.data.size(), 1);
        rapidjson::Document doc;
        doc.Parse(e.data[0]);
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_TRUE(doc.IsObject());

        EXPECT_STREQ(
            doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
            "192.168.1.1");

        auto action = e.actions.begin();
        EXPECT_EQ(e.actions.size(), 1);
        EXPECT_EQ(action->type, dds::action_type::invalid);
        EXPECT_STREQ(
            action->parameters.find("some")->second.c_str(), "parameter");
    }

    { // Using default action on waf
        std::string rules_with_actions =
            R"({"version":"2.1","rules":[{"id":"blk-001-001","name":"BlockIPAddresses","tags":{"type":"block_ip","category":"security_response"},"conditions":[{"parameters":{"inputs":[{"address":"http.client_ip"}],"data":"blocked_ips"},"operator":"ip_match"}],"transformers":[],"on_match":["block"]}], "rules_data":[{"id":"blocked_ips","type":"data_with_expiration","data":[{"value":"192.168.1.1","expiration":"9999999999"}]}]})";

        std::shared_ptr<subscriber> wi{
            waf::instance::from_string(rules_with_actions, meta, metrics)};
        ASSERT_TRUE(wi);

        auto addresses = wi->get_subscriptions();
        EXPECT_EQ(addresses.size(), 1);
        EXPECT_STREQ(addresses.begin()->c_str(), "http.client_ip");

        auto ctx = wi->get_listener();

        dds::event e;
        ctx->call(pv, e);

        EXPECT_EQ(e.data.size(), 1);
        rapidjson::Document doc;
        doc.Parse(e.data[0]);
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_TRUE(doc.IsObject());

        EXPECT_STREQ(
            doc["rule_matches"][0]["parameters"][0]["value"].GetString(),
            "192.168.1.1");

        auto action = e.actions.begin();
        EXPECT_EQ(e.actions.size(), 1);
        EXPECT_EQ(action->type, dds::action_type::block);
        EXPECT_STREQ(action->parameters.find("status_code")->second.c_str(),
            "403"); // Default value
        EXPECT_STREQ(
            action->parameters.find("grpc_status_code")->second.c_str(),
            "10"); // Default value
        EXPECT_STREQ(action->parameters.find("type")->second.c_str(),
            "auto"); // Default value
    }
}
} // namespace dds
