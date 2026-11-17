#ifndef STORAGE_H
#define STORAGE_H

#include "app.h"

#include <stdbool.h>
#include <stdio.h>

void storage_init(void);
void storage_load_mode(void);
void storage_save_mode(uint8_t mode);
void storage_seed(void);
void storage_load_sta(void);
void storage_save_sta(void);
void storage_load_logging(void);
void storage_save_logging(void);
void storage_load_project(void);
bool storage_save_project(const cango_project_t *project);
bool storage_delete_project(const char *id);
int storage_projects_json(char *buf, int cap);
int storage_logs_json(char *buf, int cap);
/* rel is relative to /sdcard. skip pages the sorted names. */
int storage_dir_json(char *buf, int cap, const char *rel, int skip);
/* NULL on success. Otherwise sd, bad, missing, busy, or rejected. */
const char *storage_delete_path(const char *rel);
/* One slice of a regular file, as one JSON line. 0 if the line does not fit. */
int storage_read_file(char *buf, int cap, const char *rel, uint32_t offset);
/* NULL on success. Otherwise sd, busy, or fail. Erases the card, then restores the empty layout. */
const char *storage_format_card(void);
/* Creates /sdcard/logs/<stamp>/ and opens log.cango inside it. */
FILE *storage_open_capture(uint32_t stamp, char *dir_out, int dir_cap);
void storage_write_census(const char *log_path, const char *json);

#endif
