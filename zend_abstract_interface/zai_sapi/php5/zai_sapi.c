#include "../zai_sapi.h"

#include <main/SAPI.h>
#include <main/php_main.h>
#include <main/php_variables.h>

#include "../zai_sapi_ini.h"
#include "../zai_sapi_io.h"

#define DEFAULT_INI        \
    "html_errors=0\n"      \
    "implicit_flush=1\n"   \
    "output_buffering=0\n" \
    "\0"

#define UNUSED(x) (void)(x)

static ssize_t ini_entries_len = -1;

static int zs_startup(sapi_module_struct *sapi_module) { return php_module_startup(sapi_module, NULL, 0); }

static int zs_deactivate(TSRMLS_D) {
#ifdef ZTS
    UNUSED(TSRMLS_C);
#endif
    return SUCCESS;
}

static void zs_send_header(sapi_header_struct *sapi_header, void *server_context TSRMLS_DC) {
    UNUSED(sapi_header);
    UNUSED(server_context);
#ifdef ZTS
    UNUSED(TSRMLS_C);
#endif
}

static char *zs_read_cookies(TSRMLS_D) {
#ifdef ZTS
    UNUSED(TSRMLS_C);
#endif
    return NULL;
}

static void zs_register_variables(zval *track_vars_array TSRMLS_DC) {
    php_import_environment_variables(track_vars_array TSRMLS_CC);
}

static int zs_io_write_stdout(const char *str, unsigned int str_length TSRMLS_DC) {
#ifdef ZTS
    UNUSED(TSRMLS_C);
#endif
    size_t len = zai_sapi_io_write_stdout(str, (size_t)str_length);
    if (len == 0) php_handle_aborted_connection();
    return (int)len;
}

static void zs_io_log_message(char *message TSRMLS_DC) {
#ifdef ZTS
    UNUSED(TSRMLS_C);
#endif
    char buf[ZAI_SAPI_IO_ERROR_LOG_MAX_BUF_SIZE];
    size_t len = zai_sapi_io_format_error_log(message, buf, sizeof buf);
    /* We ignore the return because PHP does not care if this fails. */
    (void)zai_sapi_io_write_stderr(buf, len);
}

static sapi_module_struct zai_module = {
    "zai",                     /* name */
    "Zend Abstract Interface", /* pretty name */

    zs_startup,                  /* startup */
    php_module_shutdown_wrapper, /* shutdown */

    NULL,          /* activate */
    zs_deactivate, /* deactivate */

    zs_io_write_stdout, /* unbuffered write */
    zai_sapi_io_flush,  /* flush */

    NULL, /* get uid */
    NULL, /* getenv */

    php_error, /* error handler */

    NULL,           /* header handler */
    NULL,           /* send headers handler */
    zs_send_header, /* send header handler */

    NULL,            /* read POST data */
    zs_read_cookies, /* read Cookies */

    zs_register_variables, /* register server variables */
    zs_io_log_message,     /* Log message */
    NULL,                  /* Get request time */
    NULL,                  /* Child terminate */

    NULL, /* php_ini_path_override   */
    NULL, /* block_interruptions     */
    NULL, /* unblock_interruptions   */
    NULL, /* default_post_reader     */
    NULL, /* treat_data              */
    NULL, /* executable_location     */
    0,    /* php_ini_ignore          */
    0,    /* php_ini_ignore_cwd      */
    NULL, /* get_fd                  */
    NULL, /* force_http_10           */
    NULL, /* get_target_uid          */
    NULL, /* get_target_gid          */
    NULL, /* input_filter            */
    NULL, /* ini_defaults            */
    0,    /* phpinfo_as_text;        */
    NULL, /* ini_entries;            */
    NULL, /* additional_functions    */
    NULL  /* input_filter_init       */
};

bool zai_sapi_append_system_ini_entry(const char *key, const char *value) {
    ssize_t len = zai_sapi_ini_entries_realloc_append(&zai_module.ini_entries, (size_t)ini_entries_len, key, value);
    if (len <= ini_entries_len) {
        /* Play it safe and free if writing failed. */
        zai_sapi_ini_entries_free(&zai_module.ini_entries);
        return false;
    }
    ini_entries_len = len;
    return true;
}

#ifdef ZTS
static void zs_tsrm_startup(void) {
    tsrm_startup(1, 1, 0, NULL);
    (void)ts_resource(0);
}
#endif

bool zai_sapi_sinit(void) {
#ifdef ZTS
    zs_tsrm_startup();
    TSRMLS_FETCH();
#endif

    /* Initialize the SAPI globals (memset to '0'), and set up reentrancy. */
    sapi_startup(&zai_module);

    /* Do not chdir to the script's directory (equivalent to running the CLI
     * SAPI with '-C').
     */
    SG(options) |= SAPI_OPTION_NO_CHDIR;

    /* Allocate the initial SAPI INI settings. Append new INI settings to this
     * with zai_sapi_append_system_ini_entry() before MINIT is run.
     */
    if ((ini_entries_len = zai_sapi_ini_entries_alloc(DEFAULT_INI, &zai_module.ini_entries)) == -1) return false;

    /* Don't load any INI files (equivalent to running the CLI SAPI with '-n').
     * This will prevent inadvertently loading any extensions that we did not
     * intend to. It also gives us a consistent clean slate of INI settings.
     */
    zai_module.php_ini_ignore = 1;

    /* Show phpinfo()/module info as plain text. */
    zai_module.phpinfo_as_text = 1;

    /* TODO: When we want to expose a function to userland for testing purposes
     * (e.g. DDTrace\Testing\trigger_error()), we can add them as custom SAPI
     * functions here. These functions will only exist in the ZAI SAPI for
     * testing at the C unit test level and will not be shipped as a public
     * userland API in the PHP tracer.
     */
    zai_module.additional_functions = NULL;

    return true;
}

void zai_sapi_sshutdown(void) {
    sapi_shutdown();
#ifdef ZTS
    tsrm_shutdown();
#endif
    zai_sapi_ini_entries_free(&zai_module.ini_entries);
}

bool zai_sapi_minit(void) {
    if (zai_module.startup(&zai_module) == FAILURE) {
        zai_sapi_sshutdown();
        return false;
    }
    return true;
}

void zai_sapi_mshutdown(void) {
    TSRMLS_FETCH();
    php_module_shutdown(TSRMLS_C);
}

bool zai_sapi_rinit(void) {
    TSRMLS_FETCH();
    if (php_request_startup(TSRMLS_C) == FAILURE) {
        return false;
    }

    SG(headers_sent) = 1;
    SG(request_info).no_headers = 1;

    php_register_variable("PHP_SELF", "-", NULL TSRMLS_CC);

    return true;
}

void zai_sapi_rshutdown(void) { php_request_shutdown((void *)0); }

bool zai_sapi_spinup(void) { return zai_sapi_sinit() && zai_sapi_minit() && zai_sapi_rinit(); }

void zai_sapi_spindown(void) {
    zai_sapi_rshutdown();
    zai_sapi_mshutdown();
    zai_sapi_sshutdown();
}