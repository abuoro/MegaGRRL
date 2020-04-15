#include "freertos/FreeRTOS.h"
#include "filebrowser.h"
#include "esp_log.h"
#include "lvgl.h"
#include "../key.h"
#include "../player.h"
#include "../lcddma.h"
#include "../queue.h"
#include "../taskmgr.h"
#include "../ui.h"
#include "fwupdate.h"

#include <stdio.h>
#include <dirent.h>

#include "softbar.h"

static const char* TAG = "Ui_FileBrowser";

static IRAM_ATTR lv_obj_t *container;
lv_style_t containerstyle;
lv_style_t filestyle;
lv_style_t filestyle_sel;
lv_style_t filelabelstyle_dir;
lv_style_t filelabelstyle_aud;
lv_style_t filelabelstyle_other;
lv_style_t filelabelstyle_fw;

IRAM_ATTR lv_obj_t *files[10];
IRAM_ATTR lv_obj_t *labels[10];
IRAM_ATTR lv_obj_t *icons[10];

IRAM_ATTR lv_obj_t *scrollbar;
lv_style_t scrollbarstyle;

uint16_t selectedfile = 0;
uint16_t selectedfile_last = 0xff;
uint16_t diroffset = 0;

uint8_t histdepth = 0;
uint16_t selectedfile_hist[8];
uint16_t diroffset_hist[8];

char startpath[] = "/sd";
char path[264];
char temppath[264];
DIR *dir;
struct dirent *ent;

static char direntry_cache[FILEBROWSER_CACHE_SIZE];
static IRAM_ATTR uint32_t direntry_offset[FILEBROWSER_CACHE_MAXENTRIES];
static uint16_t direntry_count = 0;

void openselection();
void redrawselection();
void redrawlist();
void startdir();

char thumbsdb[] = "Thumbs.db";
char sysvolinfo[] = "System Volume Information";
#define BROWSER_IGNORE if (ent->d_name[0] == '.' || strcasecmp(ent->d_name, thumbsdb) == 0 || strcmp(ent->d_name, sysvolinfo) == 0) continue;
#define BROWSER_LAST_VER 0x07
//uint32_t lastselectedfiletell = 0;

void savelast() {
    FILE *last;
    last = fopen("/sd/.mega/fbrowser.las", "w");
    uint8_t ver = BROWSER_LAST_VER;
    fwrite(&ver, 1, 1, last);
    uint16_t s = strlen(path);
    fwrite(&s, 2, 1, last);
    fwrite(&path, s, 1, last);
    fwrite(&histdepth, 1, 1, last); //cheezy. this just saves us from having to loop through the path to count dirs when loading
    dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "savelast(): dir is null");
        return;
    }
    char *name = direntry_cache + direntry_offset[diroffset + selectedfile];
    s = strlen(name);
    fwrite(&s, 2, 1, last);
    fwrite(name, s, 1, last);
    fclose(last);
    closedir(dir);
    ESP_LOGI(TAG, "dumped fbrowser.las");
}

void Ui_FileBrowser_Setup() {
    selectedfile_last = selectedfile;

    //this shit will ASPLODE if you're more than 8 dirs deep RIP
    //could rewrite it to iterate forward through the dirs starting at root
    FILE *last;
    last = fopen("/sd/.mega/fbrowser.las", "r");
    if (last) {
        ESP_LOGI(TAG, "fbrowser.las exists");
        uint8_t ver;
        fread(&ver, 1, 1, last);
        if (ver == BROWSER_LAST_VER) {
            ESP_LOGI(TAG, "valid version");
            uint16_t pathlen;
            fread(&pathlen, 2, 1, last);
            fread(&path, 1, pathlen, last);
            path[pathlen] = 0;
            ESP_LOGI(TAG, "read in path %s", path);
            DIR *test;
            test = opendir(path);
            if (test) {
                ESP_LOGI(TAG, "path is valid !!");
                strcpy(temppath, path);
                fread(&histdepth, 1, 1, last);
                //mitigate shit deeper than 8 dirs
                uint16_t c = 0;
                for (uint16_t i=0;i<strlen(temppath);i++) {
                    if (temppath[i] == '/') {
                        if (c++ == 8) {
                            temppath[i] = 0;
                            break;
                        }
                    }
                }
                ESP_LOGI(TAG, "truncated deep dir to %s", temppath);
                uint8_t d = histdepth-1;
                do {
                    uint16_t l = 0xffff;
                    uint16_t sl = 0xffff;
                    for (uint16_t i=0;i<strlen(temppath);i++) {
                        if (temppath[i] == '/') {
                            sl = l;
                            l = i;
                        }
                    }
                    if (strcmp(temppath, startpath) == 0) break; //reached root
                    ESP_LOGI(TAG, "finding offsets for %s", temppath);
                    DIR *finder;
                    //char bak = temppath[sl];
                    temppath[l] = 0;
                    finder = opendir(temppath);
                    uint16_t c = 0;
                    while ((ent=readdir(finder))!=NULL) {
                        BROWSER_IGNORE;
                        if (ent->d_type == DT_DIR) {
                            //ESP_LOGI(TAG, "is %s %s", ent->d_name, &temppath[l+1]);
                            if (strcmp(ent->d_name, &temppath[l+1]) == 0) {
                                diroffset_hist[d] = (c/10)*10;
                                selectedfile_hist[d] = c%10;
                                ESP_LOGI(TAG, "hist %d: diroffset %d selfile %d", d, diroffset_hist[d], selectedfile_hist[d]);
                                break;
                            }
                        }
                        c++;
                    }
                    closedir(finder);
                    d--;
                } while (strcmp(temppath, startpath));
                closedir(test);
                ESP_LOGI(TAG, "done");
                fread(&pathlen, 2, 1, last);
                fread(&temppath, 1, pathlen, last);
                temppath[pathlen] = 0;
                ESP_LOGI(TAG, "find offset for file %s", temppath);
                test = opendir(path);
                c = 0;
                while ((ent=readdir(test))!=NULL) {
                    BROWSER_IGNORE;
                    if (strcmp(ent->d_name, &temppath) == 0) {
                        diroffset = (c/10)*10;
                        selectedfile = c%10;
                        ESP_LOGI(TAG, "found file at diroffset %d selfile %d", diroffset, selectedfile);
                        break;
                    }
                    c++;
                }
                closedir(test);
                fclose(last);
                //let startdir upon activate handle the rest
                closedir(test);
                return;
            } else {
                ESP_LOGW(TAG, "path doesn't exist anymore !!");
            }
        } else {
            ESP_LOGW(TAG, "version 0x%02x doesn't match 0x%02x", ver, BROWSER_LAST_VER);
        }
        fclose(last);
    } else {
        ESP_LOGW(TAG, "no fbrowser.las");
    }

    histdepth = 0;
    selectedfile_hist[0] = 0;
    diroffset_hist[0] = 0;

    strcpy(path, startpath);
}

uint32_t filebrowser_map(uint32_t x, uint32_t in_min, uint32_t in_max, uint32_t out_min, uint32_t out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void updatescrollbar() {
    LcdDma_Mutex_Take(pdMS_TO_TICKS(1000));
    uint32_t toffset = diroffset + selectedfile;
    //direntry_count;
    uint32_t df = direntry_count;
    if (df > 100) df = 100;
    uint32_t height = filebrowser_map(df, 0, 100, 0, 215);
    height = 250-height;
    uint32_t heightleft = 250-height;
    if (height != 250) {
        uint32_t y = filebrowser_map(toffset, 0, direntry_count, 0, heightleft);
        lv_obj_set_pos(scrollbar, 235, y);
    } else {
        lv_obj_set_pos(scrollbar, 235, 0); //this is like dead code it never actually runs
    }
    lv_obj_set_height(scrollbar, height);
    LcdDma_Mutex_Give();
}

bool Ui_FileBrowser_Activate(lv_obj_t *uiscreen) {
    LcdDma_Mutex_Take(pdMS_TO_TICKS(1000));

    container = lv_cont_create(uiscreen, NULL);
    
    lv_style_copy(&containerstyle, &lv_style_plain);
    containerstyle.body.main_color = LV_COLOR_MAKE(0,0,0); //255,255,255);
    containerstyle.body.grad_color = LV_COLOR_MAKE(0,0,0); //255,255,255);
    lv_style_copy(&filestyle, &lv_style_plain);
    filestyle.body.main_color = LV_COLOR_MAKE(0,0,0); //255,255,255);
    filestyle.body.grad_color = LV_COLOR_MAKE(0,0,0); //255,255,255);
    filestyle.text.font = &lv_font_dejavu_20;
    filestyle.text.color = LV_COLOR_MAKE(220,220,220);
    lv_style_copy(&filelabelstyle_dir, &filestyle);
    lv_style_copy(&filelabelstyle_aud, &filestyle);
    lv_style_copy(&filelabelstyle_other, &filestyle);
    lv_style_copy(&filelabelstyle_fw, &filestyle);
    filelabelstyle_dir.text.color = LV_COLOR_MAKE(0xff, 0xf7, 0x69);
    filelabelstyle_aud.text.color = LV_COLOR_MAKE(0x7f, 0xff, 0xff);
    filelabelstyle_other.text.color = LV_COLOR_MAKE(0x7f, 0x7f, 0x7f);
    filelabelstyle_fw.text.color = LV_COLOR_MAKE(0, 0xff, 0);
    lv_style_copy(&filestyle_sel, &filestyle);
    filestyle_sel.body.main_color = LV_COLOR_MAKE(0,0,100); //50,50,50);
    filestyle_sel.body.grad_color = LV_COLOR_MAKE(0,0,100); //50,50,50);
    filestyle_sel.text.color = LV_COLOR_MAKE(220,220,220); //255,255,255);
    filestyle_sel.body.radius = 8;
    lv_style_copy(&scrollbarstyle, &lv_style_plain);
    scrollbarstyle.body.main_color = LV_COLOR_MAKE(127,127,127);
    scrollbarstyle.body.grad_color = LV_COLOR_MAKE(127,127,127);



    lv_cont_set_style(container, &containerstyle);
    lv_obj_set_height(container, 25*10);
    lv_obj_set_width(container, 240);
    lv_obj_set_pos(container, 0, 34+1);
    lv_cont_set_fit(container, false, false);
    //lv_obj_set_hidden(container, true);

    for (uint8_t i=0;i<10;i++) {
        files[i] = lv_cont_create(container, NULL);
        lv_cont_set_style(files[i], (i==selectedfile)?&filestyle_sel:&filestyle);
        lv_obj_set_height(files[i], 25);
        lv_obj_set_width(files[i],235);
        lv_obj_set_pos(files[i], 0, 25*i);
        labels[i] = lv_label_create(files[i], NULL);
        lv_obj_set_pos(labels[i], 26, 2);
        lv_label_set_text(labels[i], "");
        lv_label_set_long_mode(labels[i], (i==selectedfile)?LV_LABEL_LONG_ROLL:LV_LABEL_LONG_DOT);
        lv_obj_set_width(labels[i], 235-31);
        icons[i] = lv_label_create(files[i], NULL);
        lv_obj_set_pos(icons[i], 4, 2);
        lv_label_set_text(icons[i], "");
        lv_obj_set_width(icons[i], 24);
    }

    scrollbar = lv_obj_create(container, NULL);
    lv_obj_set_style(scrollbar, &scrollbarstyle);
    lv_obj_set_height(scrollbar, 250);
    lv_obj_set_width(scrollbar, 5);
    lv_obj_set_pos(scrollbar, 235, 0);


    Ui_SoftBar_Update(0, true, SYMBOL_HOME "Home", false);
    //Ui_SoftBar_Update(1, false, SYMBOL_DIRECTORY" Up "SYMBOL_UP);
    //Ui_SoftBar_Update(2, true, SYMBOL_"Open");
    LcdDma_Mutex_Give();

    startdir();

    return true;
}

void Ui_FileBrowser_Destroy() {
    LcdDma_Mutex_Take(pdMS_TO_TICKS(1000));
    lv_obj_del(container);
    LcdDma_Mutex_Give();
}

void redrawlistsel(bool list, bool sel) {
    uint8_t u=0;
    LcdDma_Mutex_Take(pdMS_TO_TICKS(1000));
    for (uint16_t i=diroffset;i<diroffset+10;i++) {
        if (i == direntry_count) break; //prevent overflow if less than 10 files left in dir
        char *name = direntry_cache + direntry_offset[i];
        unsigned char type = direntry_cache[direntry_offset[i]-1];
        if (list) {
            if (type == DT_DIR) {
                lv_label_set_static_text(icons[u], SYMBOL_DIRECTORY);
                //lv_label_set_style(labels[u], &filelabelstyle_dir);
                lv_label_set_style(icons[u], &filelabelstyle_dir);
            } else if (type == DT_REG) {
                uint8_t namelen = strlen(name);
                if (namelen > 4) {
                    if ((strcasecmp(&name[namelen-4], ".vgm") == 0) || (strcasecmp(&name[namelen-4], ".vgz") == 0)) {
                        lv_label_set_static_text(icons[u], SYMBOL_AUDIO);
                        lv_label_set_style(icons[u], &filelabelstyle_aud);
                    } else if (strcasecmp(&name[namelen-4], ".jpg") == 0 || strcasecmp(&name[namelen-4], ".png") == 0) {
                        lv_label_set_static_text(icons[u], SYMBOL_IMAGE);
                        lv_label_set_style(icons[u], &filelabelstyle_other);
                    } else if (strcasecmp(&name[namelen-4], ".txt") == 0) {
                        lv_label_set_static_text(icons[u], SYMBOL_EDIT);
                        lv_label_set_style(icons[u], &filelabelstyle_other);
                    } else if (strcasecmp(&name[namelen-4], ".m3u") == 0) {
                        lv_label_set_static_text(icons[u], SYMBOL_LIST);
                        lv_label_set_style(icons[u], &filelabelstyle_aud);
                    } else if (strcasecmp(&name[namelen-4], ".mgu") == 0) {
                        lv_label_set_static_text(icons[u], SYMBOL_DOWNLOAD);
                        lv_label_set_style(icons[u], &filelabelstyle_fw);
                    } else {
                        lv_label_set_static_text(icons[u], SYMBOL_FILE);
                        lv_label_set_style(icons[u], &filelabelstyle_other);
                    }
                } else {
                    lv_label_set_static_text(icons[u], SYMBOL_FILE);
                    lv_label_set_style(icons[u], &filelabelstyle_other);
                }
            } else {
                lv_label_set_static_text(icons[u], SYMBOL_FILE);
                lv_label_set_style(icons[u], &filelabelstyle_other);
            }
            lv_label_set_text(labels[u], name); //can't use lv_label_set_static_text here - it writes to the passed string to truncate it
        }
        if (sel) {
            if (u == selectedfile) {
                if (type == DT_DIR) {
                    Ui_SoftBar_Update(2, true, SYMBOL_RIGHT" Open", false);
                } else if (type == DT_REG) {
                    uint8_t namelen = strlen(name);
                    if (namelen > 4) {
                        char exten[5];
                        strcpy(exten, name + namelen - 4);
                        if ((strcasecmp(exten, ".vgm") == 0) || (strcasecmp(exten, ".vgz") == 0)) {
                            Ui_SoftBar_Update(2, true, SYMBOL_PLAY" Play", false);
                        } else if (strcasecmp(&name[namelen-4], ".m3u") == 0) {
                            Ui_SoftBar_Update(2, true, SYMBOL_PLAY" Play", false);
                        } else if (strcasecmp(&name[namelen-4], ".mgu") == 0) {
                            Ui_SoftBar_Update(2, true, SYMBOL_CHARGE" Flash", false);
                        } else {
                            Ui_SoftBar_Update(2, false, SYMBOL_CLOSE" N/A", false);
                        }
                    } else {
                        Ui_SoftBar_Update(2, false, SYMBOL_CLOSE" N/A", false);
                    }
                }
            }
        }
        u++;
    }
    if (list && u < 10) {
        for (uint8_t i=u;i<10;i++) {
            lv_label_set_static_text(labels[i], "");
            lv_label_set_static_text(icons[i], "");
        }
    }
    if (sel) {
        lv_cont_set_style(files[selectedfile_last], &filestyle);
        lv_label_set_long_mode(labels[selectedfile_last], LV_LABEL_LONG_DOT);
        lv_obj_set_width(labels[selectedfile_last], 240-26-2);
        lv_cont_set_style(files[selectedfile], &filestyle_sel);
        lv_label_set_long_mode(labels[selectedfile], LV_LABEL_LONG_ROLL);
        lv_obj_set_width(labels[selectedfile], 240-26-2);
        selectedfile_last = selectedfile;
    }
    LcdDma_Mutex_Give();
}

void opendirectory() {
    ESP_LOGI(TAG, "browser enter %s", path);

    if (histdepth < 8) {
        diroffset_hist[histdepth] = diroffset;
        selectedfile_hist[histdepth] = selectedfile;
    }
    histdepth++;
    diroffset = 0;
    selectedfile = 0;

    startdir();
}

uint8_t dumpm3u() {
    uint16_t m = 0;
    uint16_t r = 0;
    FILE *p;
    p = fopen("/sd/.mega/temp.m3u", "w");
    for (uint16_t i=0;i<direntry_count;i++) {
        char *name = direntry_cache + direntry_offset[i];
        unsigned char type = direntry_cache[direntry_offset[i]-1];
        if (type == DT_REG && ((strcasecmp(&name[strlen(name)-4], ".vgm") == 0) || (strcasecmp(&name[strlen(name)-4], ".vgz") == 0))) {
            strcpy(temppath, path);
            strcat(temppath, "/");
            strcat(temppath, name);
            strcat(temppath, "\n");
            fwrite(temppath, strlen(temppath), 1, p);
            if (i == diroffset + selectedfile) r = m;
            m++;
        }
    }
    fclose(p);
    return r;
}

void m3u2m3u() {
    FILE *p;
    p = fopen("/sd/.mega/temp.m3u", "w");

    QueueLoadM3u(path, temppath, 0, true);
    for (uint32_t i=0;i<QueueLength;i++) {
        QueueSetupEntry(true);
        strcat(QueuePlayingFilename, "\n"); //VILE
        fwrite(QueuePlayingFilename, strlen(QueuePlayingFilename), 1, p);
        QueueNext();
    }

    fclose(p);
}

void openselection() {
    uint16_t c = 0;
    char *name = direntry_cache + direntry_offset[diroffset + selectedfile];
    unsigned char type = direntry_cache[direntry_offset[diroffset + selectedfile]-1];
    if (type == DT_DIR) {
        strcat(path, "/");
        strcat(path, name);
        opendirectory();
        return;
    } else if (type == DT_REG) {
        if ((strcasecmp(&name[strlen(name)-4], ".vgm") == 0) || (strcasecmp(&name[strlen(name)-4], ".vgz") == 0)) {
            ESP_LOGI(TAG, "playing vgm");
            ESP_LOGI(TAG, "request stop");
            xTaskNotify(Taskmgr_Handles[TASK_PLAYER], PLAYER_NOTIFY_STOP_RUNNING, eSetValueWithoutOverwrite);
            ESP_LOGI(TAG, "wait stop");
            xEventGroupWaitBits(Player_Status, PLAYER_STATUS_NOT_RUNNING, false, true, pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "load m3u");
            QueueLoadM3u("/sd/.mega", "/sd/.mega/temp.m3u", dumpm3u(), false);
            ESP_LOGI(TAG, "request start");
            xTaskNotify(Taskmgr_Handles[TASK_PLAYER], PLAYER_NOTIFY_START_RUNNING, eSetValueWithoutOverwrite);
            Ui_Screen = UISCREEN_NOWPLAYING;
            /*ESP_LOGI(TAG, "wait start");
            xEventGroupWaitBits(Player_Status, PLAYER_STATUS_RUNNING, false, true, pdMS_TO_TICKS(3000));*/
            ESP_LOGI(TAG, "ok");
            savelast();
        } else if (strcasecmp(&name[strlen(name)-4], ".m3u") == 0) {
            ESP_LOGI(TAG, "playing m3u");
            ESP_LOGI(TAG, "request stop");
            xTaskNotify(Taskmgr_Handles[TASK_PLAYER], PLAYER_NOTIFY_STOP_RUNNING, eSetValueWithoutOverwrite);
            ESP_LOGI(TAG, "wait stop");
            xEventGroupWaitBits(Player_Status, PLAYER_STATUS_NOT_RUNNING, false, true, pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "updating m3u");
            strcpy(temppath, path);
            strcat(temppath, "/");
            strcat(temppath, name);
            m3u2m3u();
            ESP_LOGI(TAG, "load m3u");
            QueueLoadM3u("/sd/.mega", "/sd/.mega/temp.m3u", 0, false);
            ESP_LOGI(TAG, "request start");
            xTaskNotify(Taskmgr_Handles[TASK_PLAYER], PLAYER_NOTIFY_START_RUNNING, eSetValueWithoutOverwrite);
            Ui_Screen = UISCREEN_NOWPLAYING;
            /*ESP_LOGI(TAG, "wait start");
            xEventGroupWaitBits(Player_Status, PLAYER_STATUS_RUNNING, false, true, pdMS_TO_TICKS(3000));*/
            ESP_LOGI(TAG, "ok");
            savelast();
        } else if (strcasecmp(&name[strlen(name)-4], ".mgu") == 0) {
            strcpy(temppath, path);
            strcat(temppath, "/");
            strcat(temppath, name);
            fwupdate_file = temppath;
            Ui_Screen = UISCREEN_FWUPDATE;
        } else {
            ESP_LOGE(TAG, "invalid file type not played");
        }
        return;
    }
}

void backdir() {
    uint16_t l = 0xffff;
    for (uint16_t i=0;i<strlen(path);i++) {
        if (path[i] == '/') l = i;
    }
    path[l] = 0;

    ESP_LOGI(TAG, "browser enter %s", path);

    if (histdepth < 8) {
        diroffset_hist[histdepth] = diroffset;
        selectedfile_hist[histdepth] = selectedfile;
    }
    histdepth--;
    diroffset = diroffset_hist[histdepth];
    selectedfile = selectedfile_hist[histdepth];

    startdir();
}

static int comp(const uint32_t *offset1, const uint32_t *offset2) {
    char *s1 = direntry_cache + *offset1;
    char *s2 = direntry_cache + *offset2;
    unsigned char t1 = direntry_cache[*offset1-1];
    unsigned char t2 = direntry_cache[*offset2-1];
    if (t1 != t2) return t2-t1; //cheezy
    return strcasecmp(s1, s2);
}

void startdir() {
    ESP_LOGI(TAG, "generating dir cache...");
    uint32_t s = xthal_get_ccount();
    dir = opendir(path);
    uint32_t off = 0;
    direntry_count = 0;
    while ((ent=readdir(dir))!=NULL) {
        BROWSER_IGNORE;
        direntry_cache[off] = ent->d_type;
        off += 1;
        direntry_offset[direntry_count] = off;
        strcpy(&direntry_cache[off], ent->d_name);
        off += strlen(ent->d_name) + 1;
        direntry_count++;
        if (off > FILEBROWSER_CACHE_SIZE) {
            ESP_LOGE(TAG, "filebrowser cache area over !!");
            closedir(dir);
            return;
        }
        if (direntry_count > FILEBROWSER_CACHE_MAXENTRIES) {
            ESP_LOGE(TAG, "filebrowser cache over !!");
            closedir(dir);
            return;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "done after %d msec. %d entries, cache use %d", ((xthal_get_ccount()-s)/240000), direntry_count, off);
    qsort(direntry_offset, direntry_count, sizeof(uint32_t), comp);
    ESP_LOGI(TAG, "sorted");
    redrawlistsel(true, true);
    updatescrollbar();
    Ui_SoftBar_Update(1, strcmp(path, startpath) != 0, SYMBOL_UP" "SYMBOL_DIRECTORY"Up", true);
}

void Ui_FileBrowser_Key(KeyEvent_t event) {
    bool sel = false;
    bool list = false;
    if (event.State & KEY_EVENT_PRESS) {
        if (event.Key == KEY_UP) {
            if (selectedfile > 0) {
                selectedfile--;
                sel = true;
            } else if (diroffset > 0) {
                diroffset -= 10;
                selectedfile = 9;
                ESP_LOGI(TAG, "prev pg");
                sel = list = true;
            }
        } else if (event.Key == KEY_DOWN) {
            if (selectedfile < 9) {
                if (diroffset + selectedfile + 1 /*+1, would normally do direntry_count-1, but avoids potentially underflowing direntry_count*/ < direntry_count) {
                    selectedfile++;
                    sel = true;
                }
            } else if (diroffset + selectedfile + 1 < direntry_count) {
                diroffset += 10;
                selectedfile = 0;
                ESP_LOGI(TAG, "next pg");
                sel = list = true;
            }
        } else if (event.Key == KEY_RIGHT) { //todo: broken
            if (selectedfile < 9 && diroffset + selectedfile + 1 /*+1, would normally do direntry_count-1, but avoids potentially underflowing direntry_count*/ < direntry_count) {
                if (direntry_count - diroffset <= 9) {
                    selectedfile = direntry_count - diroffset - 1;
                } else {
                    selectedfile = 9;
                }
                sel = true;
            } else if (diroffset + selectedfile + 1 < direntry_count) {
                diroffset += 10;
                if (direntry_count - diroffset <= 9) {
                    selectedfile = direntry_count - diroffset - 1;
                } else {
                    selectedfile = 9;
                }
                sel = list = true;
            }
        } else if (event.Key == KEY_LEFT) {
            if (selectedfile > 0) {
                selectedfile = 0;
                sel = true;
            } else if (diroffset > 0) {
                selectedfile = 0;
                diroffset -= 10;
                sel = list = true;
            }
        } else if (event.Key == KEY_C) {
            KeyMgr_Consume(KEY_C);
            openselection();
        } else if (event.Key == KEY_B) {
            KeyMgr_Consume(KEY_B);
            if (strcmp(path, startpath) != 0) backdir();
        } else if (event.Key == KEY_A) {
            KeyMgr_Consume(KEY_A);
            Ui_Screen = UISCREEN_MAINMENU;
        }

        if (list || sel) { //if page or selection was changed
            redrawlistsel(list, sel);
            updatescrollbar();
        }
    }
}

void Ui_FileBrowser_Tick() {
    //unused atm
}