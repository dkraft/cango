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
FILE *storage_open_log(const char *dir, uint32_t stamp, char *path_out, int path_cap);
void storage_write_census(const char *log_path, const char *json);

#endif
