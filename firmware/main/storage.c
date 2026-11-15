#include "storage.h"

#include "board.h"
#include "wifi_seed.h"

#include "cango_project.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "storage";
static nvs_handle_t s_nvs;

static void seed_generic_project(void);

static void ensure_dir(const char *path)
{
    mkdir(path, 0775);
}

/* Skip names that do not fit. A truncated path would open or list the wrong file. */
static bool join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t nd = strlen(dir);
    size_t nn = strlen(name);
    if (nd + 1 + nn + 1 > cap) {
        return false;
    }
    memcpy(dst, dir, nd);
    dst[nd] = '/';
    memcpy(dst + nd + 1, name, nn + 1);
    return true;
}

static void nvs_get_str_key(const char *key, char *out, size_t cap, const char *fallback)
{
    size_t n = cap;
    if (nvs_get_str(s_nvs, key, out, &n) != ESP_OK) {
        snprintf(out, cap, "%s", fallback);
    }
}

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (nvs_open("cango", NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open");
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    host.max_freq_khz = CANGO_SD_HZ / 1000;
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = host.slot;
    if (spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return;
    }
    if (esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card) != ESP_OK) {
        ESP_LOGW(TAG, "sd mount failed");
        return;
    }
    g_app.sd_ok = true;
    ensure_dir("/sdcard/www");
    ensure_dir("/sdcard/projects");
    ensure_dir("/sdcard/logs");
    ensure_dir("/sdcard/logs/startup");
    ensure_dir("/sdcard/logs/dealer");
    ensure_dir("/sdcard/logs/manual");
    seed_generic_project();
    ESP_LOGI(TAG, "sd ready");
}

/* Factory Generic is not user-writable. Recreate the file only when it is missing. */
static void seed_generic_project(void)
{
    FILE *f;
    cango_project_t project;
    char body[768];
    int n;
    f = fopen("/sdcard/projects/generic.json", "r");
    if (f != NULL) {
        fclose(f);
        return;
    }
    cango_project_generic(&project);
    n = cango_project_write(&project, body, (int)sizeof(body));
    if (n < 0) {
        return;
    }
    f = fopen("/sdcard/projects/generic.json", "w");
    if (f == NULL) {
        return;
    }
    fwrite(body, 1, (size_t)n, f);
    fclose(f);
}

void storage_load_mode(void)
{
    uint8_t mode = CANGO_MODE_CAPTURE;
    if (nvs_get_u8(s_nvs, "mode", &mode) != ESP_OK) {
        mode = CANGO_MODE_CAPTURE;
    }
    if (mode > CANGO_MODE_WIFI) {
        mode = CANGO_MODE_CAPTURE;
    }
    g_app.mode = mode;
}

void storage_save_mode(uint8_t mode)
{
    nvs_set_u8(s_nvs, "mode", mode);
    nvs_commit(s_nvs);
}

void storage_load_sta(void)
{
    size_t n = sizeof(g_app.sta);
    g_app.sta_n = 0;
    if (nvs_get_blob(s_nvs, "sta", g_app.sta, &n) == ESP_OK && n >= sizeof(cango_sta_t)) {
        g_app.sta_n = (int)(n / sizeof(cango_sta_t));
        if (g_app.sta_n > CANGO_STA_MAX) {
            g_app.sta_n = CANGO_STA_MAX;
        }
    }
}

void storage_save_sta(void)
{
    if (g_app.sta_n < 0) {
        g_app.sta_n = 0;
    }
    if (g_app.sta_n > CANGO_STA_MAX) {
        g_app.sta_n = CANGO_STA_MAX;
    }
    nvs_set_blob(s_nvs, "sta", g_app.sta, (size_t)g_app.sta_n * sizeof(cango_sta_t));
    nvs_commit(s_nvs);
}

void storage_seed(void)
{
    uint8_t seeded = 0;
    if (nvs_get_u8(s_nvs, "sta_seed", &seeded) == ESP_OK && seeded == 1 && g_app.sta_n > 0) {
        return;
    }
    memset(g_app.sta, 0, sizeof(g_app.sta));
    snprintf(g_app.sta[0].ssid, sizeof(g_app.sta[0].ssid), "%s", CANGO_STA_1_SSID);
    snprintf(g_app.sta[0].pass, sizeof(g_app.sta[0].pass), "%s", CANGO_STA_1_PASS);
    snprintf(g_app.sta[1].ssid, sizeof(g_app.sta[1].ssid), "%s", CANGO_STA_2_SSID);
    snprintf(g_app.sta[1].pass, sizeof(g_app.sta[1].pass), "%s", CANGO_STA_2_PASS);
    g_app.sta_n = 2;
    storage_save_sta();
    nvs_set_u8(s_nvs, "sta_seed", 1);
    nvs_commit(s_nvs);
}

void storage_load_logging(void)
{
    cango_project_generic(&g_app.project);
    g_app.logging = g_app.project.logging;
    g_app.bitrate = g_app.project.bitrate;
    g_app.listen_only = true;
    uint32_t v = 0;
    uint8_t profile = CANGO_PROFILE_DEALER;
    if (nvs_get_u8(s_nvs, "prof", &profile) == ESP_OK && profile <= CANGO_PROFILE_BOTH) {
        g_app.logging.profile = profile;
    }
    if (nvs_get_u32(s_nvs, "smax", &v) == ESP_OK) {
        g_app.logging.startup_max_us = v;
    }
    if (nvs_get_u32(s_nvs, "squiet", &v) == ESP_OK) {
        g_app.logging.startup_quiet_us = v;
    }
    if (nvs_get_u32(s_nvs, "dquiet", &v) == ESP_OK) {
        g_app.logging.dealer_quiet_us = v;
    }
    if (nvs_get_u32(s_nvs, "dctx", &v) == ESP_OK) {
        g_app.logging.dealer_context_us = v;
    }
    if (nvs_get_u32(s_nvs, "uevt", &v) == ESP_OK) {
        g_app.logging.complete_on_user_events = v;
    }
    nvs_get_str_key("proj", g_app.project.id, sizeof(g_app.project.id), "generic");
}

void storage_save_logging(void)
{
    nvs_set_u8(s_nvs, "prof", g_app.logging.profile);
    nvs_set_u32(s_nvs, "smax", g_app.logging.startup_max_us);
    nvs_set_u32(s_nvs, "squiet", g_app.logging.startup_quiet_us);
    nvs_set_u32(s_nvs, "dquiet", g_app.logging.dealer_quiet_us);
    nvs_set_u32(s_nvs, "dctx", g_app.logging.dealer_context_us);
    nvs_set_u32(s_nvs, "uevt", g_app.logging.complete_on_user_events);
    nvs_set_str(s_nvs, "proj", g_app.project.id);
    nvs_commit(s_nvs);
}

static void read_project_file(const char *path, cango_project_t *out)
{
    FILE *f = fopen(path, "r");
    char buf[1024];
    size_t n;
    if (f == NULL) {
        return;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cango_project_parse(buf, out);
}

void storage_load_project(void)
{
    char path[96];
    cango_project_t loaded;
    if (!g_app.sd_ok) {
        return;
    }
    if (cango_project_is_generic(g_app.project.id)) {
        return;
    }
    snprintf(path, sizeof(path), "/sdcard/projects/%s.json", g_app.project.id);
    cango_project_generic(&loaded);
    read_project_file(path, &loaded);
    if (!cango_project_is_generic(loaded.id) && loaded.id[0]) {
        g_app.project = loaded;
        g_app.logging = loaded.logging;
        g_app.bitrate = loaded.bitrate;
    }
}

bool storage_save_project(const cango_project_t *project)
{
    char path[96];
    char body[768];
    FILE *f;
    int n;
    if (!g_app.sd_ok || project == NULL || cango_project_is_generic(project->id)) {
        return false;
    }
    n = cango_project_write(project, body, (int)sizeof(body));
    if (n < 0) {
        return false;
    }
    snprintf(path, sizeof(path), "/sdcard/projects/%s.json", project->id);
    f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }
    fwrite(body, 1, (size_t)n, f);
    fclose(f);
    return true;
}

bool storage_delete_project(const char *id)
{
    char path[96];
    if (id == NULL || cango_project_is_generic(id) || strchr(id, '/') != NULL) {
        return false;
    }
    snprintf(path, sizeof(path), "/sdcard/projects/%s.json", id);
    return remove(path) == 0;
}

static int append_written_project(char *buf, int cap, int used, const cango_project_t *project)
{
    char body[768];
    int n;
    int comma;
    if (cango_project_write(project, body, (int)sizeof(body)) < 0) {
        return -1;
    }
    n = (int)strlen(body);
    if (n > 0 && body[n - 1] == '\n') {
        n--;
    }
    comma = (used > 0 && buf[used - 1] != '[') ? 1 : 0;
    if (used + comma + n + 4 >= cap) {
        return -1;
    }
    if (comma) {
        buf[used++] = ',';
    }
    memcpy(buf + used, body, (size_t)n);
    used += n;
    buf[used] = 0;
    return used;
}

int storage_projects_json(char *buf, int cap)
{
    int used = 0;
    DIR *d;
    struct dirent *de;
    int count = 0;
    if (cap < 16) {
        return -1;
    }
    used = snprintf(buf, (size_t)cap, "{\"op\":\"get_projects\",\"ok\":true,\"projects\":[");
    d = g_app.sd_ok ? opendir("/sdcard/projects") : NULL;
    if (d != NULL) {
        while ((de = readdir(d)) != NULL) {
            char path[160];
            char body[900];
            FILE *f;
            size_t n;
            int w;
            size_t len = strlen(de->d_name);
            if (len < 6 || strcmp(de->d_name + len - 5, ".json") != 0) {
                continue;
            }
            if (!join_path(path, sizeof(path), "/sdcard/projects", de->d_name)) {
                continue;
            }
            f = fopen(path, "r");
            if (f == NULL) {
                continue;
            }
            n = fread(body, 1, sizeof(body) - 1, f);
            fclose(f);
            body[n] = 0;
            while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r')) {
                body[--n] = 0;
            }
            w = snprintf(buf + used, (size_t)(cap - used), "%s%s", count ? "," : "", body);
            if (w < 0 || used + w >= cap) {
                closedir(d);
                return -1;
            }
            used += w;
            count++;
        }
        closedir(d);
    }
    if (count == 0) {
        cango_project_t p;
        cango_project_generic(&p);
        used = append_written_project(buf, cap, used, &p);
        if (used < 0) {
            return -1;
        }
    }
    if (strstr(buf, "\"id\":\"zero-xb\"") == NULL) {
        cango_project_t p;
        if (!cango_project_builtin("zero-xb", &p)) {
            return -1;
        }
        used = append_written_project(buf, cap, used, &p);
        if (used < 0) {
            return -1;
        }
    }
    if (used + 3 >= cap) {
        return -1;
    }
    memcpy(buf + used, "]}\n", 3);
    used += 2;
    buf[used] = 0;
    return used;
}

static int append_dir(char *buf, int cap, int used, int *count, const char *dir, const char *kind)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (d == NULL) {
        return used;
    }
    while ((de = readdir(d)) != NULL) {
        char path[160];
        struct stat st;
        int w;
        if (de->d_name[0] == '.') {
            continue;
        }
        if (!join_path(path, sizeof(path), dir, de->d_name)) {
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        w = snprintf(buf + used, (size_t)(cap - used), "%s{\"name\":\"%s\",\"kind\":\"%s\",\"bytes\":%ld}",
                     *count ? "," : "", de->d_name, kind, (long)st.st_size);
        if (w < 0 || used + w >= cap) {
            break;
        }
        used += w;
        (*count)++;
    }
    closedir(d);
    return used;
}

int storage_logs_json(char *buf, int cap)
{
    int used;
    int count = 0;
    used = snprintf(buf, (size_t)cap, "{\"op\":\"list_logs\",\"ok\":true,\"logs\":[");
    if (used < 0) {
        return -1;
    }
    used = append_dir(buf, cap, used, &count, "/sdcard/logs/dealer", "dealer");
    used = append_dir(buf, cap, used, &count, "/sdcard/logs/startup", "startup");
    used = append_dir(buf, cap, used, &count, "/sdcard/logs/manual", "manual");
    if (used + 3 >= cap) {
        return -1;
    }
    memcpy(buf + used, "]}\n", 3);
    used += 2;
    buf[used] = 0;
    return used;
}

FILE *storage_open_log(const char *dir, uint32_t stamp, char *path_out, int path_cap)
{
    FILE *f;
    if (!g_app.sd_ok) {
        return NULL;
    }
    snprintf(path_out, (size_t)path_cap, "%s/%" PRIu32 ".cango", dir, stamp);
    f = fopen(path_out, "wb");
    return f;
}

void storage_write_census(const char *log_path, const char *json)
{
    char path[160];
    FILE *f;
    if (log_path == NULL || json == NULL) {
        return;
    }
    snprintf(path, sizeof(path), "%s", log_path);
    {
        char *dot = strrchr(path, '.');
        if (dot == NULL) {
            return;
        }
        snprintf(dot, sizeof(path) - (size_t)(dot - path), ".json");
    }
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fputs(json, f);
    fclose(f);
}
