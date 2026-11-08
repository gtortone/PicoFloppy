/*
 * nav.c
 *
 * Image navigation: slots, directory scanning and sorting, IMAGE_A.CFG /
 * HXCSDFE.CFG handling, and choosing the image to mount.
 *
 * Split out of main.c. Written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

static const char image_a[] = "IMAGE_A.CFG";
static const char init_image_a[] = "INIT_A.CFG";

struct nav_cfg cfg;

/* Hack inside the guts of FatFS. */
void flashfloppy_fill_fileinfo(FIL *fp);

bool_t lba_within_fat_volume(uint32_t lba)
{
    /* Also disallows access to the boot/bpb sector of the mounted volume. */
    return (lba > fatfs.volbase) && (lba <= fatfs.volend);
}

bool_t slot_valid(unsigned int i)
{
    if (i > cfg.max_slot_nr)
        return FALSE;
    if (!cfg.hxc_mode)
        return TRUE;
    if (i >= (sizeof(cfg.slot_map)*8))
        return FALSE;
    return !!(cfg.slot_map[i/8] & (0x80>>(i&7)));
}

uint16_t get_slot_nr(void)
{
    return cfg.slot_nr;
}

bool_t set_slot_nr(uint16_t slot_nr)
{
    if (!slot_valid(slot_nr))
        return FALSE;
    cfg.slot_nr = slot_nr;
    cfg.dirty_slot_nr = TRUE;
    return TRUE;
}

void set_slot_name(const char *name)
{
    snprintf(cfg.slot.name, sizeof(cfg.slot.name), "%s", name);
    cfg.dirty_slot_name = TRUE;
}

bool_t slot_type(const char *str)
{
    char ext[8];
    filename_extension(cfg.slot.name, ext, sizeof(ext));
    if (!strcmp(ext, str))
        return TRUE;
    return !strcmp(cfg.slot.type, str);
}

static void fix_hxc_short_slot(struct short_slot *short_slot)
{
    char *dot;
    /* Get rid of trailing file extension. */
    short_slot->name[51] = '\0';
    if (((dot = strrchr(short_slot->name, '.')) != NULL)
        && !strcmp(dot+1, short_slot->type))
        *dot = '\0';
}

static void slot_from_short_slot(
    struct slot *slot, const struct short_slot *short_slot)
{
    memcpy(slot->name, short_slot->name, sizeof(short_slot->name));
    slot->name[sizeof(short_slot->name)] = '\0';
    memcpy(slot->type, short_slot->type, sizeof(short_slot->type));
    slot->type[sizeof(short_slot->type)] = '\0';
    slot->attributes = short_slot->attributes;
    slot->firstCluster = short_slot->firstCluster;
    slot->size = short_slot->size;
    slot->dir_sect = slot->dir_ptr = 0;
}

static void fatfs_to_short_slot(
    struct short_slot *slot, FIL *file, const char *name)
{
    char *dot;
    unsigned int i;

    slot->attributes = file->obj.attr;
    slot->firstCluster = file->obj.sclust;
    slot->size = file->obj.objsize;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    if ((dot = strrchr(slot->name, '.')) != NULL) {
        memcpy(slot->type, dot+1, sizeof(slot->type));
        for (i = 0; i < sizeof(slot->type); i++)
            slot->type[i] = tolower(slot->type[i]);
        *dot = '\0';
    } else {
        memset(slot->type, 0, sizeof(slot->type));
    }
}

void fatfs_from_slot(FIL *file, const struct slot *slot, BYTE mode)
{
    memset(file, 0, sizeof(*file));
    file->obj.fs = &fatfs;
    file->obj.id = fatfs.id;
    file->obj.attr = slot->attributes;
    file->obj.sclust = slot->firstCluster;
    file->obj.objsize = slot->size;
    file->flag = mode;
    file->dir_sect = slot->dir_sect;
    file->dir_ptr = (void *)slot->dir_ptr;
}

static void fatfs_to_slot(struct slot *slot, FIL *file, const char *name)
{
    char *dot;
    unsigned int i;

    slot->attributes = file->obj.attr;
    slot->firstCluster = file->obj.sclust;
    slot->size = file->obj.objsize;
    slot->dir_sect = file->dir_sect;
    slot->dir_ptr = (uint32_t)file->dir_ptr;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    if ((dot = strrchr(slot->name, '.')) != NULL) {
        snprintf(slot->type, sizeof(slot->type), "%s", dot+1);
        for (i = 0; i < sizeof(slot->type); i++)
            slot->type[i] = tolower(slot->type[i]);
        *dot = '\0';
    } else {
        memset(slot->type, 0, sizeof(slot->type));
    }
}

bool_t get_img_cfg(struct slot *slot)
{
    if (!cfg.imgcfg.size)
        return FALSE;
    slot_from_short_slot(slot, &cfg.imgcfg);
    return TRUE;
}

static void dump_file(void)
{
    F_lseek(&fs->file, 0);
#if LEVEL == LEVEL_debug
    printk("[");
    do {
        F_read(&fs->file, fs->buf, sizeof(fs->buf), NULL);
        printk("%s", fs->buf);
    } while (!f_eof(&fs->file));
    printk("]\n");
    F_lseek(&fs->file, 0);
#endif
}

static bool_t native_dir_next(void)
{
    for (;;) {
        F_readdir(&fs->dp, &fs->fp);
        if (fs->fp.fname[0] == '\0')
            return FALSE;
        /* Skip dot files. */
        if (fs->fp.fname[0] == '.')
            continue;
        /* Skip hidden files/folders. */
        if (fs->fp.fattrib & AM_HID)
            continue;
        /* Allow folder navigation when LCD/OLED display is attached. */
        if ((fs->fp.fattrib & AM_DIR) && (display_type == DT_LCD_OLED)
            /* Skip FF/ in root folder */
            && ((cfg.depth != 0) || strcmp(fs->fp.fname, "FF"))
            /* Skip __MACOSX/ zip-file resource-fork folder */
            && strcmp(fs->fp.fname, "__MACOSX"))
            break;
        /* Allow valid image files. */
        if (image_valid(&fs->fp))
            break;
    }
    return TRUE;
}

static inline int __tolower(int c)
{
    if ((c >= 'A') && (c <= 'Z'))
        c += 'a' - 'A';
    return c;
}

int strcmp_lower(const char *s1, const char *s2)
{
    for (;;) {
        int diff = __tolower(*s1) - __tolower(*s2);
        if (diff || !*s1)
            return diff;
        s1++; s2++;
    }
    return 0;
}

static int native_dir_cmp(const void *a, const void *b)
{
    const struct native_dirent *da = a;
    const struct native_dirent *db = b;
    if ((da->attr ^ db->attr) & AM_DIR) {
        switch (ff_cfg.sort_priority) {
        case SORTPRI_folders:
            return (da->attr & AM_DIR) ? -1 : 1;
        case SORTPRI_files:
            return (da->attr & AM_DIR) ? 1 : -1;
        }
    }
    return strcmp_lower(da->name, db->name);
}

/* Returns -1 if not read & sorted. */
static int native_read_and_sort_dir(void)
{
    struct native_dirent **p_ent;
    struct native_dirent *ent = arena_alloc(0);
    char *start = arena_alloc(0);
    char *end = start + arena_avail();
    int nr;

    if (ff_cfg.folder_sort == SORT_never)
        return -1;

    volume_cache_destroy();

    F_opendir(&fs->dp, "");
    p_ent = (struct native_dirent **)end;
    while (((char *)p_ent - (char *)ent)
           > (FF_MAX_LFN + 1 + sizeof(*ent) + sizeof(ent))) {
        if (!native_dir_next())
            goto complete;
        *--p_ent = ent;
        ASSERT((unsigned int)(fs->fp.dir_ptr - fatfs.win) < 512u);
        ent->dir_sect = fs->fp.dir_sect;
        ent->dir_off = fs->fp.dir_ptr - fatfs.win;
        ent->attr = fs->fp.fattrib;
        strcpy(ent->name, fs->fp.fname);
        ent = (struct native_dirent *)(
            ((uint32_t)ent + sizeof(*ent) + strlen(ent->name) + 1 + 3) & ~3);
    }

    if (ff_cfg.folder_sort == SORT_always)
        goto complete;

    volume_cache_init(start, end);
    cfg.sorted = NULL;
    return -1;

complete:
    nr = (struct native_dirent **)end - p_ent;

    qsort_p(p_ent, nr, native_dir_cmp);

    F_closedir(&fs->dp);

    volume_cache_init(ent, p_ent);
    cfg.sorted = p_ent;
    return nr;
}

void update_slot_by_name(void)
{
    const char *name = cfg.slot.name;
    int len = strnlen(name, 256);
    int nr = ~0, max;

    if (cfg.sorted) {

        max = cfg.max_slot_nr;
        if (cfg.depth)
            max--;
        for (nr = 0; nr <= max; nr++)
            if (!strncmp(cfg.sorted[nr]->name, name, len))
                break;
        if (cfg.depth)
            nr++;

    }

    else if (!cfg.hxc_mode) {

        nr = cfg.depth ? 1 : 0;
        F_opendir(&fs->dp, "");
        while (native_dir_next() && strncmp(fs->fp.fname, name, len))
            nr++;
        F_closedir(&fs->dp);

    } else if (ff_cfg.nav_mode != NAVMODE_indexed) {

        struct _hxc{
            struct hxcsdfe_cfg cfg;
            struct v1_slot v1_slot;
            struct v2_slot v2_slot;
        } *hxc = (struct _hxc *)fs->buf;
        struct slot *slot = (struct slot *)hxc;
        
        slot_from_short_slot(slot, &cfg.hxcsdfe);
        fatfs_from_slot(&fs->file, slot, FA_READ);
        F_read(&fs->file, &hxc->cfg, sizeof(hxc->cfg), NULL);
        if (hxc->cfg.index_mode)
            goto out;
        for (nr = 1; nr <= cfg.max_slot_nr; nr++) {
            if (!slot_valid(nr))
                continue;
            switch (hxc->cfg.signature[9]-'0') {
            case 1:
                F_lseek(&fs->file, 1024 + nr*128);
                F_read(&fs->file, &hxc->v1_slot, sizeof(hxc->v1_slot), NULL);
                memcpy(&hxc->v2_slot.type, &hxc->v1_slot.name[8], 3);
                memcpy(&hxc->v2_slot.attributes, &hxc->v1_slot.attributes,
                       1+4+4+17);
                hxc->v2_slot.name[17] = '\0';
                break;
            case 2:
                F_lseek(&fs->file, hxc->cfg.slots_position*512
                    + nr*64*hxc->cfg.number_of_drive_per_slot);
                F_read(&fs->file, &hxc->v2_slot, sizeof(hxc->v2_slot), NULL);
                break;
            }
            fix_hxc_short_slot(&hxc->v2_slot);
            if (!strncmp(hxc->v2_slot.name, name,
                         min_t(int, len, sizeof(hxc->v2_slot.name))))
                break;
        }

    }

    set_slot_nr(nr);
out:
    cfg.dirty_slot_nr = TRUE;
}

void cfg_init(void)
{
    struct ff_cfg old_ff_cfg = ff_cfg;
    struct hxcsdfe_cfg *hxc_cfg;
    unsigned int sofar;
    char *p;
    FRESULT fr;

    memset(&cfg.clipboard, 0, sizeof(cfg.clipboard));
    cfg.dirty_slot_nr = FALSE;
    cfg.dirty_slot_name = FALSE;
    cfg.hxc_mode = FALSE;
    cfg.ima_ej_flag = FALSE;
    cfg.slot_nr = cfg.depth = 0;
    cfg.cur_cdir = fatfs.cdir;

    fr = f_chdir("FF");
    cfg.cfg_cdir = fatfs.cdir;

    memset(&cfg.imgcfg, 0, sizeof(cfg.imgcfg));
    fr = F_try_open(&fs->file, "IMG.CFG", FA_READ);
    if (!fr) {
        fatfs_to_short_slot(&cfg.imgcfg, &fs->file, "IMG.CFG");
        F_close(&fs->file);
    }

    read_ff_cfg();
    process_ff_cfg_opts(&old_ff_cfg);

    switch (ff_cfg.nav_mode) {
    case NAVMODE_native:
        goto native_mode;
    case NAVMODE_indexed:
        cfg.hxc_mode = TRUE;
        goto out;
    default:
        break;
    }

    /* Probe for HxC compatibility mode. */
    fatfs.cdir = cfg.cur_cdir;
    fr = F_try_open(&fs->file, "HXCSDFE.CFG", FA_READ|FA_WRITE);
    if (fr)
        goto native_mode;
    fatfs_to_short_slot(&cfg.hxcsdfe, &fs->file, "HXCSDFE.CFG");
    hxc_cfg = (struct hxcsdfe_cfg *)fs->buf;
    F_read(&fs->file, hxc_cfg, sizeof(*hxc_cfg), NULL);
    if (hxc_cfg->startup_mode & HXCSTARTUP_slot0) {
        /* Startup mode: slot 0. */
        hxc_cfg->slot_index = hxc_cfg->cur_slot_number = 0;
        F_lseek(&fs->file, 0);
        F_write(&fs->file, hxc_cfg, sizeof(*hxc_cfg), NULL);
    }
    if (hxc_cfg->startup_mode & HXCSTARTUP_ejected) {
        /* Startup mode: eject. */
        cfg.ejected = TRUE;
    }
    hxc_cfg = NULL;
    F_close(&fs->file);

    /* Slot 0 is a dummy image unless AUTOBOOT.HFE exists. */
    memset(&cfg.autoboot, 0, sizeof(cfg.autoboot));
    snprintf(cfg.autoboot.name, sizeof(cfg.autoboot.name), "(Empty)");
    cfg.autoboot.firstCluster = ~0u; /* flag to dummy_open() */

    fr = F_try_open(&fs->file, "AUTOBOOT.HFE", FA_READ);
    if (!fr) {
        fatfs_to_short_slot(&cfg.autoboot, &fs->file, "AUTOBOOT.HFE");
        cfg.autoboot.attributes |= AM_RDO; /* default read-only */
        F_close(&fs->file);
    }

    cfg.hxc_mode = TRUE;
    goto out;

native_mode:
    /* Native mode (direct navigation). */
    fatfs.cdir = cfg.cfg_cdir;

    sofar = 0;
    if (ff_cfg.image_on_startup == IMGS_static) {
        fr = F_try_open(&fs->file, init_image_a, FA_READ);
        if (!fr) {
            sofar = min_t(unsigned int, f_size(&fs->file), sizeof(fs->buf));
            F_read(&fs->file, fs->buf, sofar, NULL);
            F_close(&fs->file);
        }
    }

    F_open(&fs->file, image_a, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);

    if (ff_cfg.image_on_startup != IMGS_last) {
        F_lseek(&fs->file, 0);
        F_truncate(&fs->file);
        F_write(&fs->file, fs->buf, sofar, NULL);
        F_lseek(&fs->file, 0);
    }

    /* Process IMAGE_A.CFG file. */
    sofar = 0; /* bytes consumed so far */
    fatfs.cdir = cfg.cur_cdir;
    lcd_write(0, 3, -1, "/");
    for (;;) {
        int nr;
        bool_t ok;
        /* Read next pathname section, search for its terminating slash. */
        F_read(&fs->file, fs->buf, sizeof(fs->buf), NULL);
        fs->buf[sizeof(fs->buf)-1] = '\0';
        for (p = fs->buf; *p && (*p != '/'); p++)
            continue;
        /* No terminating slash: we're done. */
        if ((p == fs->buf) || !*p)
            break;
        /* Terminate the name section, push curdir onto stack, then chdir. */
        *p++ = '\0';
        printk("%u:D: '%s'\n", cfg.depth, fs->buf);
        lcd_write(0, 3, -1, fs->buf);
        if (cfg.depth == ARRAY_SIZE(cfg.stack))
            F_die(FR_PATH_TOO_DEEP);
        /* Find slot nr, and stack it */
        if ((nr = native_read_and_sort_dir()) != -1) {
            while (--nr >= 0)
                if (!strcmp(cfg.sorted[nr]->name, fs->buf))
                    break;
            ok = (nr >= 0);
            cfg.sorted = NULL;
        } else {
            F_opendir(&fs->dp, "");
            while ((ok = native_dir_next()) && strcmp(fs->fp.fname, fs->buf))
                nr++;
            F_closedir(&fs->dp);
        }
        if (!ok)
            goto clear_image_a;
        nr += cfg.depth ? 1 : 0;
        cfg.stack[cfg.depth].slot = nr;
        cfg.stack[cfg.depth++].cdir = fatfs.cdir;
        fr = f_chdir(fs->buf);
        if (fr)
            goto clear_image_a;
        /* Seek on to next pathname section. */
        sofar += p - fs->buf;
        F_lseek(&fs->file, sofar);
    }
    if (cfg.depth != 0) {
        /* No subfolder support on LED display. */
        if (display_type != DT_LCD_OLED)
            goto clear_image_a; /* no subfolder support on LED display */
        /* Skip '..' entry. */
        cfg.slot_nr = 1;
    }
    while ((p != fs->buf) && isspace(p[-1]))
        *--p = '\0'; /* Strip trailing whitespace */
    if (((p - fs->buf) >= 3) && !strcmp(p-3, "\\EJ")) {
        /* Eject flag "\\EJ" is found. Act on it and then skip over it. */
        cfg.ejected = TRUE;
        cfg.ima_ej_flag = TRUE;
        p -= 3;
        *p = '\0';
    }
    if (p != fs->buf) {
        /* If there was a non-empty non-terminated pathname section, it 
         * must be the name of the currently-selected image file. */
        bool_t ok;
        int i, nr;
        printk("%u:F: '%s' %s\n", cfg.depth, fs->buf,
               cfg.ima_ej_flag ? "(EJ)" : "");
        native_get_slot_map(TRUE);
        cfg.slot_nr = cfg.depth ? 1 : 0;
        if (cfg.sorted) {
            nr = cfg.max_slot_nr + 1 - cfg.slot_nr;
            for (i = 0; i < nr; i++)
                if (!strcmp(cfg.sorted[i]->name, fs->buf))
                    break;
            ok = (i < nr);
            cfg.slot_nr += i;
        } else {
            F_opendir(&fs->dp, "");
            while ((ok = native_dir_next()) && strcmp(fs->fp.fname, fs->buf))
                cfg.slot_nr++;
            F_closedir(&fs->dp);
        }
        if (!ok)
            goto clear_image_a;
    }
    F_close(&fs->file);
    cfg.cur_cdir = fatfs.cdir;

out:
    printk("Mode: %s\n", cfg.hxc_mode ? "HxC" : "Native");
    fatfs.cdir = cfg.cur_cdir;
    return;

clear_image_a:
    /* Error! Clear the IMAGE_A.CFG file. */
    printk("IMAGE_A.CFG is bad: clearing it\n");
    F_lseek(&fs->file, 0);
    lcd_write(0, 3, -1, "/");
    F_truncate(&fs->file);
    F_close(&fs->file);
    cfg.slot_nr = cfg.depth = 0;
    cfg.ima_ej_flag = FALSE;
    cfg.sorted = NULL;
    goto out;
}

void native_get_slot_map(bool_t sorted_only)
{
    int i;

    /* Populate slot_map[]. */
    memset(&cfg.slot_map, 0xff, sizeof(cfg.slot_map));
    cfg.max_slot_nr = cfg.depth ? 1 : 0;

    if ((i = native_read_and_sort_dir()) != -1) {
        cfg.max_slot_nr += i;
    } else {
        if (sorted_only)
            return;
        F_opendir(&fs->dp, "");
        while (native_dir_next())
            cfg.max_slot_nr++;
        F_closedir(&fs->dp);
    }

    /* Adjust max_slot_nr. Must be at least one 'slot'. */
    if (!cfg.max_slot_nr)
        F_die(FR_NO_DIRENTS);
    cfg.max_slot_nr--;

    /* Select last disk_index if not greater than available slots. */
    cfg.slot_nr = (cfg.slot_nr <= cfg.max_slot_nr) ? cfg.slot_nr : 0;
}


static void native_update(uint8_t slot_mode)
{
    int i;

    if ((slot_mode == CFG_READ_SLOT_NR) && !cfg.sorted)
        native_get_slot_map(FALSE);

    if (slot_mode == CFG_WRITE_SLOT_NR) {
        char *p, *q;
        fatfs.cdir = cfg.cfg_cdir;
        F_open(&fs->file, image_a, FA_READ|FA_WRITE);
        printk("Before: "); dump_file();
        /* Read final section of the file. */
        if (f_size(&fs->file) > sizeof(fs->buf))
            F_lseek(&fs->file, f_size(&fs->file) - sizeof(fs->buf));
        F_read(&fs->file, fs->buf, sizeof(fs->buf), NULL);
        F_lseek(&fs->file, (f_size(&fs->file) > sizeof(fs->buf)
                            ? f_size(&fs->file) - sizeof(fs->buf) : 0));
        /* Find end of last subfolder name, if any. */
        if ((p = strrchr(fs->buf, '/')) != NULL) {
            /* Found: seek to after the trailing '/'. */
            F_lseek(&fs->file, f_tell(&fs->file) + (p+1 - fs->buf));
        } else {
            /* No subfolder: we overwrite the entire file. */
            F_lseek(&fs->file, 0);
        }
        if (cfg.slot.attributes & AM_DIR) {
            if (!strcmp(fs->fp.fname, "..")) {
                /* Strip back to next '/' */
                if (!p) F_die(FR_BAD_IMAGECFG); /* must exist */
                *p = '\0';
                if ((q = strrchr(fs->buf, '/')) != NULL) {
                    /* We need to find the next preceding '/' so we can update 
                     * multi-row LCD/OLED display with subfolder info. */
                    long sz, pos = f_tell(&fs->file) - (p-q);
                    /* pos-1 = offset of end of subfolder name. */
                    sz = min_t(long, pos-1, sizeof(fs->buf));
                    F_lseek(&fs->file, pos-1-sz);
                    F_read(&fs->file, fs->buf, sz, NULL);
                    fs->buf[sz] = '\0';
                    /* q = pointer to '/' preceding the subfolder name. */
                    q = strrchr(fs->buf, '/');
                    lcd_write(0, 3, -1, q ? q+1 : fs->buf);
                    F_lseek(&fs->file, pos);
                } else {
                    F_lseek(&fs->file, 0);
                    lcd_write(0, 3, -1, "/");
                }
            } else {
                /* Add name plus '/' */
                F_write(&fs->file, fs->fp.fname,
                        strnlen(fs->fp.fname, sizeof(fs->fp.fname)), NULL);
                F_write(&fs->file, "/", 1, NULL);
                lcd_write(0, 3, -1, fs->fp.fname);
            }
        } else {
            /* Add name */
            F_write(&fs->file, fs->fp.fname,
                    strnlen(fs->fp.fname, sizeof(fs->fp.fname)), NULL);
        }
        F_truncate(&fs->file);
        printk("After: "); dump_file();
        F_close(&fs->file);
        fatfs.cdir = cfg.cur_cdir;
        cfg.ima_ej_flag = FALSE;
    }
    
    /* Populate current slot. */
    if ((i = cfg.depth ? 1 : 0) > cfg.slot_nr) {
        /* Must be the ".." folder. */
        snprintf(fs->fp.fname, sizeof(fs->fp.fname), "..");
        fs->fp.fattrib = AM_DIR;
        goto is_dir;
    }

    if (cfg.sorted) {

        struct native_dirent *ent = cfg.sorted[cfg.slot_nr-i];
        snprintf(fs->fp.fname, sizeof(fs->fp.fname), ent->name);
        fs->file.obj.fs = &fatfs;
        fs->file.dir_sect = ent->dir_sect;
        fs->file.dir_ptr = fatfs.win + ent->dir_off;
        flashfloppy_fill_fileinfo(&fs->file);
        fs->fp.fattrib = fs->file.obj.attr;
        if (fs->file.obj.attr & AM_DIR)
            goto is_dir;
        fatfs_to_slot(&cfg.slot, &fs->file, fs->fp.fname);

    } else {

        F_opendir(&fs->dp, "");
        while (native_dir_next()) {
            if (i >= cfg.slot_nr)
                break;
            i++;
        }
        F_closedir(&fs->dp);
        if (fs->fp.fattrib & AM_DIR) {
        is_dir:
            /* Leave the full pathname cached in fs->fp. */
            cfg.slot.attributes = fs->fp.fattrib;
            snprintf(cfg.slot.name, sizeof(cfg.slot.name),
                     "[%s]", fs->fp.fname);
        } else {
            F_open(&fs->file, fs->fp.fname, FA_READ);
            fs->file.obj.attr = fs->fp.fattrib;
            fatfs_to_slot(&cfg.slot, &fs->file, fs->fp.fname);
            F_close(&fs->file);
        }

    }
}

void ima_mark_ejected(bool_t ej)
{
    if (cfg.hxc_mode || (cfg.ima_ej_flag == ej))
        return;

    fatfs.cdir = cfg.cfg_cdir;
    F_open(&fs->file, image_a, FA_READ|FA_WRITE);
    printk("Before: "); dump_file();
    if (ej) {
        F_lseek(&fs->file, f_size(&fs->file));
        F_write(&fs->file, "\\EJ", 3, NULL);
    } else {
        F_lseek(&fs->file, max_t(int, f_size(&fs->file)-3, 0));
        F_truncate(&fs->file);
    }
    printk("After: "); dump_file();
    F_close(&fs->file);
    fatfs.cdir = cfg.cur_cdir;
    cfg.ima_ej_flag = ej;
}

static void hxc_cfg_update(uint8_t slot_mode)
{
    struct _hxc {
        struct hxcsdfe_cfg cfg;
        struct v1_slot v1_slot;
        struct v2_slot v2_slot;
    } *hxc = (struct _hxc *)fs->buf;
    BYTE mode = FA_READ;
    int i;

    if (slot_mode == CFG_WRITE_SLOT_NR)
        mode |= FA_WRITE;

    if (ff_cfg.nav_mode == NAVMODE_indexed) {
        FRESULT fr;
        char slot[10];
        hxc->cfg.index_mode = TRUE;
        fatfs.cdir = cfg.cfg_cdir;
        switch (slot_mode) {
        case CFG_READ_SLOT_NR:
            cfg.slot_nr = 0;
            if (ff_cfg.image_on_startup == IMGS_init)
                break;
            if ((fr = F_try_open(&fs->file, image_a, FA_READ)) != FR_OK)
                break;
            F_read(&fs->file, slot, sizeof(slot), NULL);
            F_close(&fs->file);
            slot[sizeof(slot)-1] = '\0';
            cfg.slot_nr = strtol(slot, NULL, 10);
            break;
        case CFG_WRITE_SLOT_NR:
            if (ff_cfg.image_on_startup != IMGS_last)
                break;
            snprintf(slot, sizeof(slot), "%u", cfg.slot_nr);
            F_open(&fs->file, image_a, FA_WRITE | FA_OPEN_ALWAYS);
            F_write(&fs->file, slot, strnlen(slot, sizeof(slot)), NULL);
            F_truncate(&fs->file);
            F_close(&fs->file);
            break;
        }
        fatfs.cdir = cfg.cur_cdir;
        goto indexed_mode;
    }

    slot_from_short_slot(&cfg.slot, &cfg.hxcsdfe);
    fatfs_from_slot(&fs->file, &cfg.slot, mode);
    F_read(&fs->file, &hxc->cfg, sizeof(hxc->cfg), NULL);
    if (strncmp("HXCFECFGV", hxc->cfg.signature, 9))
        goto bad_signature;

    if (slot_mode == CFG_READ_SLOT_NR) {
        /* buzzer_step_duration seems to range 0xFF-0xD8. */
        if (!cfg.ffcfg_has_step_volume)
            ff_cfg.step_volume = hxc->cfg.step_sound
                ? (0x100 - hxc->cfg.buzzer_step_duration) / 2 : 0;
        if (!cfg.ffcfg_has_display_off_secs)
            ff_cfg.display_off_secs = hxc->cfg.back_light_tmr;
        /* Interpret HxC scroll speed as updates per minute. */
        if (!cfg.ffcfg_has_display_scroll_rate && hxc->cfg.lcd_scroll_speed)
            ff_cfg.display_scroll_rate = 60000u / hxc->cfg.lcd_scroll_speed;
    }

    switch (hxc->cfg.signature[9]-'0') {

    case 1: {
        if (slot_mode != CFG_READ_SLOT_NR) {
            /* Keep the already-configured slot number. */
            hxc->cfg.slot_index = cfg.slot_nr;
            if (slot_mode == CFG_WRITE_SLOT_NR) {
                /* Update the config file with new slot number. */
                F_lseek(&fs->file, 0);
                F_write(&fs->file, &hxc->cfg, sizeof(hxc->cfg), NULL);
            }
        }
        cfg.slot_nr = hxc->cfg.slot_index;
        if (hxc->cfg.index_mode)
            break;
        /* Slot mode: initialise slot map and current slot. */
        if (slot_mode == CFG_READ_SLOT_NR) {
            cfg.max_slot_nr = hxc->cfg.number_of_slot - 1;
            memset(&cfg.slot_map, 0xff, sizeof(cfg.slot_map));
        }
        /* Slot mode: read current slot file info. */
        if (cfg.slot_nr == 0) {
            slot_from_short_slot(&cfg.slot, &cfg.autoboot);
        } else {
            F_lseek(&fs->file, 1024 + cfg.slot_nr*128);
            F_read(&fs->file, &hxc->v1_slot, sizeof(hxc->v1_slot), NULL);
            memcpy(&hxc->v2_slot.type, &hxc->v1_slot.name[8], 3);
            memcpy(&hxc->v2_slot.attributes, &hxc->v1_slot.attributes,
                   1+4+4+17);
            hxc->v2_slot.name[17] = '\0';
            fix_hxc_short_slot(&hxc->v2_slot);
            slot_from_short_slot(&cfg.slot, &hxc->v2_slot);
        }
        break;
    }

    case 2:
        if (slot_mode != CFG_READ_SLOT_NR) {
            hxc->cfg.cur_slot_number = cfg.slot_nr;
            if (slot_mode == CFG_WRITE_SLOT_NR) {
                F_lseek(&fs->file, 0);
                F_write(&fs->file, &hxc->cfg, sizeof(hxc->cfg), NULL);
            }
        }
        cfg.slot_nr = hxc->cfg.cur_slot_number;
        if (hxc->cfg.index_mode)
            break;
        /* Slot mode: initialise slot map and current slot. */
        if (slot_mode == CFG_READ_SLOT_NR) {
            cfg.max_slot_nr = hxc->cfg.max_slot_number - 1;
            F_lseek(&fs->file, hxc->cfg.slots_map_position*512);
            F_read(&fs->file, &cfg.slot_map, sizeof(cfg.slot_map), NULL);
            cfg.slot_map[0] |= 0x80; /* slot 0 always available */
            /* Find true max_slot_nr: */
            while (!slot_valid(cfg.max_slot_nr))
                cfg.max_slot_nr--;
        }
        /* Slot mode: read current slot file info. */
        if (cfg.slot_nr == 0) {
            slot_from_short_slot(&cfg.slot, &cfg.autoboot);
        } else if (slot_valid(cfg.slot_nr)) {
            F_lseek(&fs->file, hxc->cfg.slots_position*512
                    + cfg.slot_nr*64*hxc->cfg.number_of_drive_per_slot);
            F_read(&fs->file, &hxc->v2_slot, sizeof(hxc->v2_slot), NULL);
            fix_hxc_short_slot(&hxc->v2_slot);
            slot_from_short_slot(&cfg.slot, &hxc->v2_slot);
        } else {
            memset(&cfg.slot, 0, sizeof(cfg.slot));
        }
        break;

    default:
    bad_signature:
        hxc->cfg.signature[15] = '\0';
        printk("Bad signature '%s'\n", hxc->cfg.signature);
        F_die(FR_BAD_HXCSDFE);

    }

    F_close(&fs->file);

indexed_mode:
    if (hxc->cfg.index_mode) {

        char name[16];

        /* Index mode: populate slot_map[]. */
        if (slot_mode == CFG_READ_SLOT_NR) {
            memset(&cfg.slot_map, 0, sizeof(cfg.slot_map));
            cfg.max_slot_nr = 0;
            snprintf(name, sizeof(name), "%s*.*", ff_cfg.indexed_prefix);
            for (F_findfirst(&fs->dp, &fs->fp, "", name);
                 fs->fp.fname[0] != '\0';
                 F_findnext(&fs->dp, &fs->fp)) {
                const char *p = fs->fp.fname
                    + strnlen(ff_cfg.indexed_prefix,
                            sizeof(ff_cfg.indexed_prefix));
                unsigned int idx = 0;
                /* Skip directories. */
                if (fs->fp.fattrib & AM_DIR)
                    continue;
                /* Parse 4-digit index number. */
                for (i = 0; i < 4; i++) {
                    if ((*p < '0') || (*p > '9'))
                        break;
                    idx *= 10;
                    idx += *p++ - '0';
                }
                /* Expect a 4-digit number range 0-999. */
                if ((i != 4) || (idx > 999))
                    continue;
                /* A file type we support? */
                if (!image_valid(&fs->fp))
                    continue;
                /* All is fine, populate the 'slot'. */
                cfg.slot_map[idx/8] |= 0x80 >> (idx&7);
                cfg.max_slot_nr = max_t(
                    uint16_t, cfg.max_slot_nr, idx);
            }
            F_closedir(&fs->dp);
            if (!slot_valid(cfg.max_slot_nr))
                F_die(FR_NO_DIRENTS);
        }

        /* Index mode: populate current slot. */
        snprintf(name, sizeof(name), "%s%04u*.*",
                 ff_cfg.indexed_prefix, cfg.slot_nr);
        printk("[%s]\n", name);
        F_findfirst(&fs->dp, &fs->fp, "", name);
        F_closedir(&fs->dp);
        if (fs->fp.fname[0]) {
            /* Found a valid image. */
            F_open(&fs->file, fs->fp.fname, FA_READ);
            fs->file.obj.attr = fs->fp.fattrib;
            fatfs_to_slot(&cfg.slot, &fs->file, fs->fp.fname);
            F_close(&fs->file);
        } else {
            memset(&cfg.slot, 0, sizeof(cfg.slot));
        }
    }

    for (i = 0; i < sizeof(cfg.slot.type); i++)
        cfg.slot.type[i] = tolower(cfg.slot.type[i]);
}

/* Always updates cfg.slot info for current slot_nr. Additionally:
 * Native (Direct Navigation):
 *  READ_SLOT:  Update slot_map/max_slot_nr for current directory.
 *  WRITE_SLOT: Update IMAGE_A.CFG based on cached fs->fp.fname.
 * NAVMODE_indexed (FF-native indexed mode):
 *  READ_SLOT:  Update slot_nr from IMAGE_A.CFG. Update slot_map/max_slot_nr.
 *  WRITE_SLOT: Update IMAGE_A.CFG from slot_nr.
 * HxC Indexed Mode:
 *  READ_SLOT:  Update slot_nr from HXCSDFE.CFG. Update slot_map/max_slot_nr.
 *  WRITE_SLOT: Update HXCSDFE.CFG from slot_nr.
 * HxC Selector Mode:
 *  READ_SLOT:  Update slot_nr/slot_map/max_slot_nr from HXCSDFE.CFG.
 *  WRITE_SLOT: Update HXCSDFE.CFG from slot_nr. */
void cfg_update(uint8_t slot_mode)
{
    if (cfg.hxc_mode)
        hxc_cfg_update(slot_mode);
    else
        native_update(slot_mode);
    if (!(cfg.slot.attributes & AM_DIR)
        && (ff_cfg.write_protect || volume_readonly()))
        cfg.slot.attributes |= AM_RDO;
}

static void htu_choose_new_image(uint8_t b)
{
    int digits[3], pos, i;
    time_t t = time_now();

    /* Isolate the digits of the current slot number. */
    i = cfg.slot_nr;
    for (pos = 0; pos < 3; pos++) {
        digits[pos] = i % 10;
        i /= 10;
    }

    /* Tens or Units depending on which button is pressed. */
    pos = !(b & B_RIGHT);

    do {
        if (!(b ^ (B_LEFT|B_RIGHT))) {
            /* Both Buttons: Hundreds. */
            pos = 2;
            if (time_since(t) > time_ms(1000)) {
                /* After 1sec with both buttons held, immediately update the
                 * display to show the reset to 000. */
                i = 0;
                goto out;
            }
        }
    } while ((b = buttons&(B_LEFT|B_RIGHT)) != 0);

    /* Increment the selected digit. */
    digits[pos] = (digits[pos] + 1) % 10;

    /* Reconstitute the slot number. Reset the selected digit on overflow. */
    i = digits[0] + digits[1]*10 + digits[2]*100;
    if (i > cfg.max_slot_nr) {
        digits[pos] = 0;
        i = digits[0] + digits[1]*10 + digits[2]*100;
    }

out:
    cfg.slot_nr = i;
    cfg_update(CFG_KEEP_SLOT_NR);
    display_write_slot(TRUE);
    while (b && buttons)
        continue;
}

/* Based on button presses, change which floppy image is selected. */
bool_t choose_new_image(uint8_t init_b)
{
    uint8_t b, prev_b;
    time_t last_change = 0;
    int old_slot = cfg.slot_nr, i, changes = 0;
    uint8_t twobutton_action = ff_cfg.twobutton_action & TWOBUTTON_mask;

    for (prev_b = 0, b = init_b;
         (b &= (B_LEFT|B_RIGHT)) != 0;
         prev_b = b, b = buttons) {

        if (prev_b == b) {
            /* Decaying delay between image steps while button pressed. */
            time_t delay = time_ms(1000) / (changes + 1);
            if (delay < time_ms(50))
                delay = time_ms(50);
            if (twobutton_action == TWOBUTTON_rotary_fast)
                delay = time_ms(40);
            if (time_diff(last_change, time_now()) < delay)
                continue;
            changes++;
        } else {
            /* Different button pressed. Takes immediate effect, resets 
             * the continuous-press decaying delay. */
            changes = 0;
        }
        last_change = time_now();

        if (twobutton_action == TWOBUTTON_htu) {
            htu_choose_new_image(b);
            return FALSE;
        }

        i = cfg.slot_nr;
        if (!(b ^ (B_LEFT|B_RIGHT))) {
            if (twobutton_action == TWOBUTTON_eject) {
                cfg.slot_nr = old_slot;
                cfg.ejected = TRUE;
                cfg_update(CFG_KEEP_SLOT_NR);
                break;
            }
            i = cfg.slot_nr = 0;
            cfg_update(CFG_KEEP_SLOT_NR);
            if ((twobutton_action == TWOBUTTON_rotary)
                || (twobutton_action == TWOBUTTON_rotary_fast)) {
                /* Wait for button release, then update display, or
                 * immediately enter parent-dir (if we're in a subfolder). */
                while (buttons)
                    continue;
                if (cfg.depth == 0)
                    display_write_slot(TRUE);
                return (cfg.depth != 0);
            }
            display_write_slot(TRUE);
            /* Ignore changes while user is releasing the buttons. */
            while ((time_diff(last_change, time_now()) < time_ms(1000))
                   && buttons)
                continue;
        } else if (b & B_LEFT) {
            i -= velocity ?: 1;
            if ((i < 0) && !ff_cfg.nav_loop) {
                i = 0;
                goto b_right;
            }
            while (i < 0)
                i += cfg.max_slot_nr + 1;
        b_left:
            while (!slot_valid(i)) {
                if (i-- == 0) {
                    if (!ff_cfg.nav_loop)
                        goto b_right;
                    i = cfg.max_slot_nr;
                }
            } while (!slot_valid(i));
        } else { /* b & B_RIGHT */
            i += velocity ?: 1;
            if ((i > cfg.max_slot_nr) && !ff_cfg.nav_loop) {
                i = cfg.max_slot_nr;
                goto b_left;
            }
            i = i % (cfg.max_slot_nr + 1);
        b_right:
            while (!slot_valid(i)) {
                if (i++ >= cfg.max_slot_nr) {
                    if (!ff_cfg.nav_loop)
                        goto b_left;
                    i = 0;
                }
            }
        }

        cfg.slot_nr = i;
        cfg_update(CFG_KEEP_SLOT_NR);
        display_write_slot(TRUE);
    }

    return FALSE;
}
