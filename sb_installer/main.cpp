#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/Net.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/libkernel.h>

// Dialog types
#define MDIALOG_OK       0
#define MDIALOG_YESNO    1

// Progress bar target
#define ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT 0


static inline void _orbisCommonDialogSetMagicNumber(uint32_t* magic, const OrbisCommonDialogBaseParam* param)
{
    *magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)param);
}

static inline void _orbisCommonDialogBaseParamInit(OrbisCommonDialogBaseParam *param)
{
    memset(param, 0x0, sizeof(OrbisCommonDialogBaseParam));
    param->size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    _orbisCommonDialogSetMagicNumber(&(param->magic), param);
}

static inline void orbisMsgDialogParamInitialize(OrbisMsgDialogParam *param)
{
    memset(param, 0x0, sizeof(OrbisMsgDialogParam));
    _orbisCommonDialogBaseParamInit(&param->baseParam);
    param->size = sizeof(OrbisMsgDialogParam);
}

int show_dialog(int dialog_type, const char * format, ...)
{
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;

    char str[0x800];
    memset(str, 0, sizeof(str));

    va_list opt;
    va_start(opt, format);
    vsprintf(str, format, opt);
    va_end(opt);

    sceMsgDialogInitialize();
    orbisMsgDialogParamInitialize(&param);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;

    // Get the initial user ID
    int32_t userId = 0;
    sceUserServiceGetInitialUser(&userId);
    param.userId = userId;

    memset(&userMsgParam, 0, sizeof(userMsgParam));
    userMsgParam.msg = str;
    userMsgParam.buttonType = (dialog_type ? ORBIS_MSG_DIALOG_BUTTON_TYPE_YESNO_FOCUS_NO : ORBIS_MSG_DIALOG_BUTTON_TYPE_OK);
    param.userMsgParam = &userMsgParam;

    if (sceMsgDialogOpen(&param) < 0) {
        return 0;
    }

    OrbisCommonDialogStatus status;
    do {
        status = sceMsgDialogUpdateStatus();
        sceKernelUsleep(100000); // Sleep 100ms
    } while (status != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogClose();

    memset(&result, 0, sizeof(result));
    sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();

    return (result.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_YES);
}

void show_progress_dialog(const char* msg) {
    OrbisMsgDialogParam param;
    OrbisMsgDialogProgressBarParam progParam;

    sceMsgDialogInitialize();
    orbisMsgDialogParamInitialize(&param);

    // Get user ID
    int32_t userId = 0;
    sceUserServiceGetInitialUser(&userId);
    param.userId = userId;

    param.mode = ORBIS_MSG_DIALOG_MODE_PROGRESS_BAR;

    memset(&progParam, 0, sizeof(progParam));
    progParam.barType = ORBIS_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    progParam.msg = msg;
    param.progBarParam = &progParam;

    sceMsgDialogOpen(&param);
}

void update_progress(const char* msg, int percent) {
    sceMsgDialogProgressBarSetMsg(ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, msg);
    sceMsgDialogProgressBarSetValue(ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, percent);
    printf("%s (%d%%)\n", msg, percent);
}

void close_progress_dialog() {
    sceMsgDialogClose();

    // Wait for dialog to actually close
    do {
        sceKernelUsleep(100000);
    } while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogTerminate();
}

static int ssl_callback(int libsslId, unsigned int verifyErr, void * const sslCert[], int certNum, void *userArg) {
    return 1; // Accept all certificates
}

int download_file(const char* url, const char* output_path) {
    int libnetMemId, libsslCtxId, libhttpCtxId, tplId, connId, reqId, ret;
    uint8_t* buffer;
    uint64_t total_read = 0;
    uint64_t content_length = 0;
    int read_size;
    FILE* fd;
    char temp_msg[256];

    // Initialize network
    update_progress("Initializing network...", 0);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);

    ret = sceNetInit();
    ret = sceNetPoolCreate("netPool", 16 * 1024, 0);
    if (ret < 0) return -1;
    libnetMemId = ret;

    ret = sceSslInit(512 * 1024);
    if (ret < 0) {
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }
    libsslCtxId = ret;

    ret = sceHttpInit(libnetMemId, libsslCtxId, 512 * 1024);
    if (ret < 0) {
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }
    libhttpCtxId = ret;

    update_progress("Connecting to GitHub...", 10);
    tplId = sceHttpCreateTemplate(libhttpCtxId, "ps4-installer/1.0", ORBIS_HTTP_VERSION_1_1, 1);
    if (tplId < 0) {
        sceHttpTerm(libhttpCtxId);
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }

    sceHttpsSetSslCallback(tplId, ssl_callback, NULL);

    connId = sceHttpCreateConnectionWithURL(tplId, url, 1);
    if (connId < 0) {
        sceHttpDeleteTemplate(tplId);
        sceHttpTerm(libhttpCtxId);
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }

    reqId = sceHttpCreateRequestWithURL(connId, ORBIS_METHOD_GET, url, 0);
    if (reqId < 0) {
        sceHttpDeleteConnection(connId);
        sceHttpDeleteTemplate(tplId);
        sceHttpTerm(libhttpCtxId);
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }

    update_progress("Sending request...", 20);
    ret = sceHttpSendRequest(reqId, NULL, 0);
    if (ret < 0) {
        sceHttpDeleteRequest(reqId);
        sceHttpDeleteConnection(connId);
        sceHttpDeleteTemplate(tplId);
        sceHttpTerm(libhttpCtxId);
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }

    // Get content length
    int contentLengthType;
    sceHttpGetResponseContentLength(reqId, &contentLengthType, &content_length);

    update_progress("Opening file...", 30);
    fd = fopen(output_path, "wb");
    if (!fd) {
        sceHttpDeleteRequest(reqId);
        sceHttpDeleteConnection(connId);
        sceHttpDeleteTemplate(tplId);
        sceHttpTerm(libhttpCtxId);
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }

    buffer = (uint8_t*)malloc(64 * 1024);
    if (!buffer) {
        fclose(fd);
        sceHttpDeleteRequest(reqId);
        sceHttpDeleteConnection(connId);
        sceHttpDeleteTemplate(tplId);
        sceHttpTerm(libhttpCtxId);
        sceSslTerm();
        sceNetPoolDestroy(libnetMemId);
        return -1;
    }

    update_progress("Downloading...", 40);
    while ((read_size = sceHttpReadData(reqId, buffer, 64 * 1024)) > 0) {
        fwrite(buffer, 1, read_size, fd);
        total_read += read_size;

        // Update progress based on download
        if (content_length > 0) {
            int percent = 40 + (int)((total_read * 50) / content_length);
            snprintf(temp_msg, sizeof(temp_msg), "Downloading... %lu KB / %lu KB",
                     (unsigned long)(total_read / 1024), (unsigned long)(content_length / 1024));
            update_progress(temp_msg, percent);
        }
    }

    update_progress("Download complete", 90);

    fclose(fd);
    free(buffer);

    sceHttpDeleteRequest(reqId);
    sceHttpDeleteConnection(connId);
    sceHttpDeleteTemplate(tplId);
    sceHttpTerm(libhttpCtxId);
    sceSslTerm();
    sceNetPoolDestroy(libnetMemId);

    return 0;
}

int create_directory_recursive(const char* path) {
    char tmp[256];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
    return 0;
}

int append_to_plugins_ini(const char* config) {
    const char* ini_path = "/data/GoldHEN/plugins.ini";
    FILE* fp;

    // Check if file exists
    fp = fopen(ini_path, "r");
    if (fp) {
        // File exists, check if config already present
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "yt_adblock.prx")) {
                found = 1;
                break;
            }
        }
        fclose(fp);

        if (found) {
            return 0; // Already configured
        }
    }

    // Append config
    fp = fopen(ini_path, "a");
    if (!fp) {
        printf("Failed to open plugins.ini\n");
        return -1;
    }

    fprintf(fp, "\n%s\n", config);
    fclose(fp);

    return 0;
}

int check_if_installed(const char* plugin_path, const char* ini_path) {
    FILE* fp;
    int file_exists = 0;
    int ini_configured = 0;

    // Check if plugin file exists
    fp = fopen(plugin_path, "r");
    if (fp) {
        file_exists = 1;
        fclose(fp);
    }

    // Check if already in plugins.ini
    fp = fopen(ini_path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "yt_adblock.prx")) {
                ini_configured = 1;
                break;
            }
        }
        fclose(fp);
    }

    return (file_exists && ini_configured);
}

void install_sb(){
    const char* download_url = "https://github.com/earthonion/yt_adskip_ps4/releases/latest/download/yt_adblock.prx";
    const char* output_path = "/data/GoldHEN/plugins/yt_adblock.prx";
    const char* plugin_config = "[CUSA01015]\n/data/GoldHEN/plugins/yt_adblock.prx";
    const char* ini_path = "/data/GoldHEN/plugins.ini";

    // Check if already installed
    if (check_if_installed(output_path, ini_path)) {
        if (!show_dialog(MDIALOG_YESNO, "YouTube AdBlock is already installed.\n\nWould you like to update/reinstall?")) {
            show_dialog(MDIALOG_OK, "Installation cancelled.");
            return;
        }
    }

    // Show progress dialog
    show_progress_dialog("Installing YouTube AdBlock + Sponsorblock...");

    // Create directory if it doesn't exist
    update_progress("Creating plugin directory...", 5);
    create_directory_recursive("/data/GoldHEN/plugins");

    // Download the file
    if (download_file(download_url, output_path) < 0) {
        close_progress_dialog();
        show_dialog(MDIALOG_OK, "Installation FAILED!\n\nDownload error.");
        return;
    }

    // Update plugins.ini
    update_progress("Updating plugins.ini...", 95);
    if (append_to_plugins_ini(plugin_config) < 0) {
        close_progress_dialog();
        show_dialog(MDIALOG_OK, "Installation FAILED!\n\nFailed to update plugins.ini");
        return;
    }

    update_progress("Installation complete!", 100);
    sceKernelUsleep(500000); // Show complete for 0.5 seconds
    close_progress_dialog();

    show_dialog(MDIALOG_OK, "Installation Complete!\n\nYouTube AdBlock + Sponsorblock has been installed. Click OK to exit.");
    sceSystemServiceLoadExec("exit", NULL);
}



int main()
{
    // Load the Message Dialog module
    if (sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG) < 0 ||
        sceCommonDialogInitialize() < 0)
    {
        printf("Failed to initialize CommonDialog\n");
        for(;;);
    }

    // Show a message dialog
    if (show_dialog(MDIALOG_YESNO, "Do you want to install YouTube AdBlock + Sponsorblock?"))
    {
        install_sb();
    }
    else
    {
        show_dialog(MDIALOG_OK, "Click OK to exit.");
        sceSystemServiceLoadExec("exit", NULL);
    }
    for(;;);
}
