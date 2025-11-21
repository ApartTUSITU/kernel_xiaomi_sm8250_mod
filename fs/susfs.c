#include <linux/version.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/printk.h>
#include <linux/namei.h>
#include <linux/list.h>
#include <linux/init_task.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/fdtable.h>
#include <linux/statfs.h>
#include <linux/random.h>
#include <linux/susfs.h>
#include "mount.h"

extern bool susfs_is_current_ksu_domain(void);

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
bool susfs_is_log_enabled __read_mostly = true;
#define SUSFS_LOGI(fmt, ...) if (susfs_is_log_enabled) pr_info("susfs:[%u][%d][%s] " fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#define SUSFS_LOGE(fmt, ...) if (susfs_is_log_enabled) pr_err("susfs:[%u][%d][%s]" fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#else
#define SUSFS_LOGI(fmt, ...) 
#define SUSFS_LOGE(fmt, ...) 
#endif

bool susfs_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++)
            return false;
    }
    return true;
}

/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
static DEFINE_SPINLOCK(susfs_spin_lock_sus_path);
static LIST_HEAD(LH_SUS_PATH_LOOP);
static LIST_HEAD(LH_SUS_PATH_ANDROID_DATA);
static LIST_HEAD(LH_SUS_PATH_SDCARD);
static struct st_external_dir android_data_path = {0};
static struct st_external_dir sdcard_path = {0};
const struct qstr susfs_fake_qstr_name = QSTR_INIT("..5.u.S", 7); // used to re-test the dcache lookup, make sure you don't have file named like this!!

void susfs_set_i_state_on_external_dir(void __user **user_info) {
	struct path path;
	struct inode *inode = NULL;
	char *tmp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	char *resolved_pathname = NULL;
	static struct st_external_dir info = {0};

	if (!tmp_buf) {
		info.err = -ENOMEM;
		goto out_copy_to_user;
	}

	if (copy_from_user(&info, (struct st_external_dir __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_kfree_tmp_buf;
	}

	info.err = kern_path(info.target_pathname, LOOKUP_FOLLOW, &path);
	if (info.err) {
		SUSFS_LOGE("Failed opening file '%s'\n", info.target_pathname);
		goto out_kfree_tmp_buf;
	}

	resolved_pathname = d_path(&path, tmp_buf, PAGE_SIZE);
	if (!resolved_pathname) {
		info.err = -EINVAL;
		goto out_path_put_path;
	}

	inode = d_inode(path.dentry);
	if (!inode) {
		info.err = -EINVAL;
		goto out_path_put_path;
	}
	
	if (info.cmd == CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_ANDROID_DATA_ROOT_DIR, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
		strncpy(android_data_path.target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME-1);
		android_data_path.is_inited = true;
		android_data_path.cmd = CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH;
		SUSFS_LOGI("Set android data root dir: '%s', i_mapping: '0x%p'\n",
			android_data_path.target_pathname, inode->i_mapping);
		info.err = 0;
	} else if (info.cmd == CMD_SUSFS_SET_SDCARD_ROOT_PATH) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_SDCARD_ROOT_DIR, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
		strncpy(sdcard_path.target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME-1);
		sdcard_path.is_inited = true;
		sdcard_path.cmd = CMD_SUSFS_SET_SDCARD_ROOT_PATH;
		SUSFS_LOGI("Set sdcard root dir: '%s', i_mapping: '0x%p'\n",
			sdcard_path.target_pathname, inode->i_mapping);
		info.err = 0;
	} else {
		info.err = -EINVAL;
	}

out_path_put_path:
	path_put(&path);
out_kfree_tmp_buf:
	kfree(tmp_buf);
out_copy_to_user:
	if (copy_to_user(&((struct st_external_dir __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	if (info.cmd == CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH) {
		SUSFS_LOGI("CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH -> ret: %d\n", info.err);
	} else if (info.cmd == CMD_SUSFS_SET_SDCARD_ROOT_PATH) {
		SUSFS_LOGI("CMD_SUSFS_SET_SDCARD_ROOT_PATH -> ret: %d\n", info.err);
	}
}

void susfs_add_sus_path(void __user **user_info) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;
	struct st_susfs_sus_path_list *new_list = NULL;
	struct st_susfs_sus_path info = {0};
	struct path path;
	struct inode *inode = NULL;
	char *resolved_pathname = NULL, *tmp_buf = NULL;

	if (copy_from_user(&info, (struct st_susfs_sus_path __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	info.err = kern_path(info.target_pathname, 0, &path);
	if (info.err) {
		SUSFS_LOGE("Failed opening file '%s'\n", info.target_pathname);
		goto out_copy_to_user;
	}

	if (!path.dentry->d_inode) {
		info.err = -EINVAL;
		goto out_path_put_path;
	}
	inode = d_inode(path.dentry);

	tmp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp_buf) {
		info.err = -ENOMEM;
		goto out_path_put_path;
	}

	resolved_pathname = d_path(&path, tmp_buf, PAGE_SIZE);
	if (!resolved_pathname) {
		info.err = -ENOMEM;
		goto out_kfree_tmp_buf;
	}

	if (strstr(resolved_pathname, android_data_path.target_pathname)) {
		if (!android_data_path.is_inited) {
			info.err = -EINVAL;
			SUSFS_LOGE("android_data_path is not configured yet, plz do like 'ksu_susfs set_android_data_root_path /sdcard/Android/data' first after your screen is unlocked\n");
			goto out_kfree_tmp_buf;
		}
		list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_ANDROID_DATA, list) {
			if (unlikely(!strcmp(cursor->info.target_pathname, path.dentry->d_name.name))) {
				spin_lock(&susfs_spin_lock_sus_path);
				cursor->info.target_ino = info.target_ino;
				strncpy(cursor->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
				strncpy(cursor->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
				cursor->info.i_uid = info.i_uid;
				cursor->path_len = strlen(cursor->info.target_pathname);
				SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully updated to LH_SUS_PATH_ANDROID_DATA\n",
							cursor->info.target_ino, cursor->target_pathname, cursor->info.i_uid);
				spin_unlock(&susfs_spin_lock_sus_path);
				info.err = 0;
				goto out_kfree_tmp_buf;
			}
		}
		new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
		if (!new_list) {
			info.err = -ENOMEM;
			goto out_kfree_tmp_buf;
		}
		new_list->info.target_ino = info.target_ino;
		strncpy(new_list->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
		strncpy(new_list->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
		new_list->info.i_uid = info.i_uid;
		new_list->path_len = strlen(new_list->info.target_pathname);
		INIT_LIST_HEAD(&new_list->list);
		spin_lock(&susfs_spin_lock_sus_path);
		list_add_tail(&new_list->list, &LH_SUS_PATH_ANDROID_DATA);
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully added to LH_SUS_PATH_ANDROID_DATA\n",
					new_list->info.target_ino, new_list->target_pathname, new_list->info.i_uid);
		spin_unlock(&susfs_spin_lock_sus_path);
		info.err = 0;
		goto out_kfree_tmp_buf;
	} else if (strstr(resolved_pathname, sdcard_path.target_pathname)) {
		if (!sdcard_path.is_inited) {
			info.err = -EINVAL;
			SUSFS_LOGE("sdcard_path is not configured yet, plz do like 'ksu_susfs set_sdcard_root_path /sdcard' first after your screen is unlocked\n");
			goto out_kfree_tmp_buf;
		}
		list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_SDCARD, list) {
			if (unlikely(!strcmp(cursor->info.target_pathname, path.dentry->d_name.name))) {
				spin_lock(&susfs_spin_lock_sus_path);
				cursor->info.target_ino = info.target_ino;
				strncpy(cursor->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
				strncpy(cursor->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
				cursor->info.i_uid = info.i_uid;
				cursor->path_len = strlen(cursor->info.target_pathname);
				SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully updated to LH_SUS_PATH_SDCARD\n",
							cursor->info.target_ino, cursor->target_pathname, cursor->info.i_uid);
				spin_unlock(&susfs_spin_lock_sus_path);
				info.err = 0;
				goto out_kfree_tmp_buf;
			}
		}
		new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
		if (!new_list) {
			info.err = -ENOMEM;
			goto out_kfree_tmp_buf;
		}
		new_list->info.target_ino = info.target_ino;
		strncpy(new_list->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
		strncpy(new_list->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
		new_list->info.i_uid = info.i_uid;
		new_list->path_len = strlen(new_list->info.target_pathname);
		INIT_LIST_HEAD(&new_list->list);
		spin_lock(&susfs_spin_lock_sus_path);
		list_add_tail(&new_list->list, &LH_SUS_PATH_SDCARD);
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully added to LH_SUS_PATH_SDCARD\n",
					new_list->info.target_ino, new_list->target_pathname, new_list->info.i_uid);
		spin_unlock(&susfs_spin_lock_sus_path);
		info.err = 0;
		goto out_kfree_tmp_buf;
	}

	spin_lock(&inode->i_lock);
	set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
	SUSFS_LOGI("pathname: '%s', ino: '%lu', is flagged as AS_FLAGS_SUS_PATH\n", resolved_pathname, info.target_ino);
	spin_unlock(&inode->i_lock);
	info.err = 0;
out_kfree_tmp_buf:
	kfree(tmp_buf);
out_path_put_path:
	path_put(&path);
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_sus_path __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ADD_SUS_PATH -> ret: %d\n", info.err);
}

void susfs_add_sus_path_loop(void __user **user_info) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;
	struct st_susfs_sus_path_list *new_list = NULL;
	struct st_susfs_sus_path info = {0};
	struct path path;
	struct inode *inode = NULL;
	char *resolved_pathname = NULL, *tmp_buf = NULL;

	if (copy_from_user(&info, (struct st_susfs_sus_path __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	info.err = kern_path(info.target_pathname, 0, &path);
	if (info.err) {
		SUSFS_LOGE("Failed opening file '%s'\n", info.target_pathname);
		goto out_copy_to_user;
	}

	if (!path.dentry->d_inode) {
		info.err = -EINVAL;
		goto out_path_put_path;
	}
	inode = d_inode(path.dentry);

	tmp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp_buf) {
		info.err = -ENOMEM;
		goto out_path_put_path;
	}

	resolved_pathname = d_path(&path, tmp_buf, PAGE_SIZE);
	SUSFS_LOGI("resolved_pathname: %s\n", resolved_pathname);
	if (!resolved_pathname) {
		info.err = -ENOMEM;
		goto out_kfree_tmp_buf;
	}

	if (susfs_starts_with(resolved_pathname, "/storage/")) {
		info.err = -EINVAL;
		SUSFS_LOGE("path starts with /storage and /sdcard cannot be added by add_sus_path_loop\n");
		goto out_kfree_tmp_buf;
	}

	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_LOOP, list) {
		if (unlikely(!strcmp(cursor->info.target_pathname, resolved_pathname))) {
			spin_lock(&susfs_spin_lock_sus_path);
			cursor->info.target_ino = info.target_ino;
			strncpy(cursor->info.target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
			strncpy(cursor->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
			cursor->info.i_uid = info.i_uid;
			cursor->path_len = strlen(cursor->info.target_pathname);
			SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully updated to LH_SUS_PATH_LOOP\n",
						cursor->info.target_ino, cursor->target_pathname, cursor->info.i_uid);
			spin_unlock(&susfs_spin_lock_sus_path);
			goto out_set_sus_path;
		}
	}
	new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
	if (!new_list) {
		info.err = -ENOMEM;
		goto out_kfree_tmp_buf;
	}
	new_list->info.target_ino = info.target_ino;
	strncpy(new_list->info.target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
	strncpy(new_list->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
	new_list->info.i_uid = info.i_uid;
	new_list->path_len = strlen(new_list->info.target_pathname);
	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock_sus_path);
	list_add_tail(&new_list->list, &LH_SUS_PATH_LOOP);
	SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully added to LH_SUS_PATH_LOOP\n",
				new_list->info.target_ino, new_list->target_pathname, new_list->info.i_uid);
	spin_unlock(&susfs_spin_lock_sus_path);
out_set_sus_path:
	spin_lock(&inode->i_lock);
	set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
	SUSFS_LOGI("pathname: '%s', ino: '%lu', is flagged as AS_FLAGS_SUS_PATH\n", resolved_pathname, info.target_ino);
	spin_unlock(&inode->i_lock);
	info.err = 0;
out_kfree_tmp_buf:
	kfree(tmp_buf);
out_path_put_path:
	path_put(&path);
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_sus_path __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ADD_SUS_PATH_LOOP -> ret: %d\n", info.err);
}

void susfs_run_sus_path_loop(uid_t uid) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;
	struct path path;
	struct inode *inode;

	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_LOOP, list) {
		if (!kern_path(cursor->target_pathname, 0, &path)) {
			inode = path.dentry->d_inode;
			spin_lock(&inode->i_lock);
			set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
			spin_unlock(&inode->i_lock);
			path_put(&path);
			SUSFS_LOGI("re-flag '%s' as SUS_PATH for uid: %u\n", cursor->target_pathname, uid);
		}
	}
}

static inline bool is_i_uid_in_android_data_not_allowed(uid_t i_uid) {
	return (likely(susfs_is_current_proc_umounted()) &&
		unlikely(current_uid().val != i_uid));
}

static inline bool is_i_uid_in_sdcard_not_allowed(void) {
	return (likely(susfs_is_current_proc_umounted()));
}

static inline bool is_i_uid_not_allowed(uid_t i_uid) {
	return (likely(susfs_is_current_proc_umounted()) &&
		unlikely(current_uid().val != i_uid));
}

bool susfs_is_base_dentry_android_data_dir(struct dentry* base) {
	return (base && !IS_ERR(base) && base->d_inode && (base->d_inode->i_mapping->flags & BIT_ANDROID_DATA_ROOT_DIR));
}

bool susfs_is_base_dentry_sdcard_dir(struct dentry* base) {
	return (base && !IS_ERR(base) && base->d_inode && (base->d_inode->i_mapping->flags & BIT_ANDROID_SDCARD_ROOT_DIR));
}

bool susfs_is_sus_android_data_d_name_found(const char *d_name) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;

	if (d_name[0] == '\0') {
		return false;
	}

	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_ANDROID_DATA, list) {
		// - we use strstr here because we cannot retrieve the dentry of fuse_dentry
		//   and attacker can still use path travesal attack to detect the path, but
		//   lucky we can check for the uid so it won't let them fool us
		if (!strncmp(d_name, cursor->info.target_pathname, cursor->path_len) &&
		    (d_name[cursor->path_len] == '\0' || d_name[cursor->path_len] == '/') &&
			is_i_uid_in_android_data_not_allowed(cursor->info.i_uid))
		{
			SUSFS_LOGI("hiding path '%s'\n", cursor->target_pathname);
			return true;
		}
	}
	return false;
}

bool susfs_is_sus_sdcard_d_name_found(const char *d_name) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;

	if (d_name[0] == '\0') {
		return false;
	}
	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_SDCARD, list) {
		if (!strncmp(d_name, cursor->info.target_pathname, cursor->path_len) &&
		    (d_name[cursor->path_len] == '\0' || d_name[cursor->path_len] == '/') &&
			is_i_uid_in_sdcard_not_allowed())
		{
			SUSFS_LOGI("hiding path '%s'\n", cursor->target_pathname);
			return true;
		}
	}
	return false;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
bool susfs_is_inode_sus_path(struct mnt_idmap* idmap, struct inode *inode) {
	if (unlikely(inode->i_mapping->flags & BIT_SUS_PATH &&
		is_i_uid_not_allowed(i_uid_into_vfsuid(idmap, inode).val)))
	{
		SUSFS_LOGI("hiding path with ino '%lu'\n", inode->i_ino);
		return true;
	}
	return false;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
bool susfs_is_inode_sus_path(struct inode *inode) {
	if (unlikely(inode->i_mapping->flags & BIT_SUS_PATH &&
		is_i_uid_not_allowed(i_uid_into_mnt(i_user_ns(inode), inode).val)))
	{
		SUSFS_LOGI("hiding path with ino '%lu'\n", inode->i_ino);
		return true;
	}
	return false;
}
#else
bool susfs_is_inode_sus_path(struct inode *inode) {
	if (unlikely(inode->i_mapping->flags & BIT_SUS_PATH &&
		is_i_uid_not_allowed(inode->i_uid.val)))
	{
		SUSFS_LOGI("hiding path with ino '%lu'\n", inode->i_ino);
		return true;
	}
	return false;
}
#endif

#endif // #ifdef CONFIG_KSU_SUSFS_SUS_PATH

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
static DEFINE_SPINLOCK(susfs_spin_lock_sus_mount);
bool susfs_hide_sus_mnts_for_all_procs = true; // hide sus mounts for all processes by default

void susfs_set_hide_sus_mnts_for_all_procs(void __user **user_info) {
	struct st_susfs_hide_sus_mnts_for_all_procs info = {0};

	if (copy_from_user(&info, (struct st_susfs_hide_sus_mnts_for_all_procs __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}
	spin_lock(&susfs_spin_lock_sus_mount);
	susfs_hide_sus_mnts_for_all_procs = info.enabled;
	spin_unlock(&susfs_spin_lock_sus_mount);
	SUSFS_LOGI("susfs_hide_sus_mnts_for_all_procs: %d\n", info.enabled);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_hide_sus_mnts_for_all_procs __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_HIDE_SUS_MNTS_FOR_ALL_PROCS -> ret: %d\n", info.err);
}

bool susfs_is_umount_for_zygote_iso_service_enabled = false;
void susfs_set_umount_for_zygote_iso_service(void __user **user_info) {
	struct st_susfs_umount_for_zygote_iso_service info = {0};

	if (copy_from_user(&info, (struct st_susfs_umount_for_zygote_iso_service __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}
	spin_lock(&susfs_spin_lock_sus_mount);
	susfs_is_umount_for_zygote_iso_service_enabled = info.enabled;
	spin_unlock(&susfs_spin_lock_sus_mount);
	SUSFS_LOGI("susfs_is_umount_for_zygote_iso_service_enabled: %d\n", info.enabled);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_umount_for_zygote_iso_service __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_UMOUNT_FOR_ZYGOTE_ISO_SERVICE -> ret: %d\n", info.err);
}
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT

/* sus_kstat */
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
static DEFINE_SPINLOCK(susfs_spin_lock_sus_kstat);
static DEFINE_HASHTABLE(SUS_KSTAT_HLIST, 10);
static int susfs_update_sus_kstat_inode(char *target_pathname) {
	struct path p;
	struct inode *inode = NULL;
	int err = 0;

	err = kern_path(target_pathname, LOOKUP_FOLLOW, &p);
	if (err) {
		SUSFS_LOGE("Failed opening file '%s'\n", target_pathname);
		return 1;
	}

	inode = d_inode(p.dentry);
	if (!inode) {
		path_put(&p);
		SUSFS_LOGE("inode is NULL\n");
		return 1;
	}

	if (!(inode->i_mapping->flags & BIT_SUS_KSTAT)) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_SUS_KSTAT, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
	}
	path_put(&p);
	return 0;
}

void susfs_add_sus_kstat(void __user **user_info) {
	struct st_susfs_sus_kstat info = {0};
	struct st_susfs_sus_kstat_hlist *new_entry, *tmp_entry;
	struct hlist_node *tmp_node;
	int bkt;
	bool update_hlist = false;

	if (copy_from_user(&info, (struct st_susfs_sus_kstat __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	if (strlen(info.target_pathname) == 0) {
		info.err = -EINVAL;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_sus_kstat);
	hash_for_each_safe(SUS_KSTAT_HLIST, bkt, tmp_node, tmp_entry, node) {
		if (!strcmp(tmp_entry->info.target_pathname, info.target_pathname)) {
			hash_del(&tmp_entry->node);
			kfree(tmp_entry);
			update_hlist = true;
			break;
		}
	}
	spin_unlock(&susfs_spin_lock_sus_kstat);

	new_entry = kmalloc(sizeof(struct st_susfs_sus_kstat_hlist), GFP_KERNEL);
	if (!new_entry) {
		info.err = -ENOMEM;
		goto out_copy_to_user;
	}

#if defined(__ARCH_WANT_STAT64) || defined(__ARCH_WANT_COMPAT_STAT64)
#ifdef CONFIG_MIPS
	info.spoofed_dev = new_decode_dev(info.spoofed_dev);
#else
	info.spoofed_dev = huge_decode_dev(info.spoofed_dev);
#endif /* CONFIG_MIPS */
#else
	info.spoofed_dev = old_decode_dev(info.spoofed_dev);
#endif /* defined(__ARCH_WANT_STAT64) || defined(__ARCH_WANT_COMPAT_STAT64) */

	new_entry->target_ino = info.target_ino;
	memcpy(&new_entry->info, &info, sizeof(info));

	if (susfs_update_sus_kstat_inode(new_entry->info.target_pathname)) {
		kfree(new_entry);
		info.err = -EINVAL;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_sus_kstat);
	hash_add(SUS_KSTAT_HLIST, &new_entry->node, info.target_ino);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	if (update_hlist) {
		SUSFS_LOGI("is_statically: '%d', target_ino: '%lu', target_pathname: '%s', spoofed_ino: '%lu', spoofed_dev: '%lu', spoofed_nlink: '%u', spoofed_size: '%llu', spoofed_atime_tv_sec: '%ld', spoofed_mtime_tv_sec: '%ld', spoofed_ctime_tv_sec: '%ld', spoofed_atime_tv_nsec: '%ld', spoofed_mtime_tv_nsec: '%ld', spoofed_ctime_tv_nsec: '%ld', spoofed_blksize: '%lu', spoofed_blocks: '%llu', is successfully added to SUS_KSTAT_HLIST\n",
				new_entry->info.is_statically, new_entry->info.target_ino, new_entry->info.target_pathname,
				new_entry->info.spoofed_ino, new_entry->info.spoofed_dev,
				new_entry->info.spoofed_nlink, new_entry->info.spoofed_size,
				new_entry->info.spoofed_atime_tv_sec, new_entry->info.spoofed_mtime_tv_sec, new_entry->info.spoofed_ctime_tv_sec,
				new_entry->info.spoofed_atime_tv_nsec, new_entry->info.spoofed_mtime_tv_nsec, new_entry->info.spoofed_ctime_tv_nsec,
				new_entry->info.spoofed_blksize, new_entry->info.spoofed_blocks);
	} else {
		SUSFS_LOGI("is_statically: '%d', target_ino: '%lu', target_pathname: '%s', spoofed_ino: '%lu', spoofed_dev: '%lu', spoofed_nlink: '%u', spoofed_size: '%llu', spoofed_atime_tv_sec: '%ld', spoofed_mtime_tv_sec: '%ld', spoofed_ctime_tv_sec: '%ld', spoofed_atime_tv_nsec: '%ld', spoofed_mtime_tv_nsec: '%ld', spoofed_ctime_tv_nsec: '%ld', spoofed_blksize: '%lu', spoofed_blocks: '%llu', is successfully updated to SUS_KSTAT_HLIST\n",
				new_entry->info.is_statically, new_entry->info.target_ino, new_entry->info.target_pathname,
				new_entry->info.spoofed_ino, new_entry->info.spoofed_dev,
				new_entry->info.spoofed_nlink, new_entry->info.spoofed_size,
				new_entry->info.spoofed_atime_tv_sec, new_entry->info.spoofed_mtime_tv_sec, new_entry->info.spoofed_ctime_tv_sec,
				new_entry->info.spoofed_atime_tv_nsec, new_entry->info.spoofed_mtime_tv_nsec, new_entry->info.spoofed_ctime_tv_nsec,
				new_entry->info.spoofed_blksize, new_entry->info.spoofed_blocks);
	}
#else
	if (update_hlist) {
		SUSFS_LOGI("is_statically: '%d', target_ino: '%lu', target_pathname: '%s', spoofed_ino: '%lu', spoofed_dev: '%lu', spoofed_nlink: '%u', spoofed_size: '%u', spoofed_atime_tv_sec: '%ld', spoofed_mtime_tv_sec: '%ld', spoofed_ctime_tv_sec: '%ld', spoofed_atime_tv_nsec: '%ld', spoofed_mtime_tv_nsec: '%ld', spoofed_ctime_tv_nsec: '%ld', spoofed_blksize: '%lu', spoofed_blocks: '%llu', is successfully added to SUS_KSTAT_HLIST\n",
				new_entry->info.is_statically, new_entry->info.target_ino, new_entry->info.target_pathname,
				new_entry->info.spoofed_ino, new_entry->info.spoofed_dev,
				new_entry->info.spoofed_nlink, new_entry->info.spoofed_size,
				new_entry->info.spoofed_atime_tv_sec, new_entry->info.spoofed_mtime_tv_sec, new_entry->info.spoofed_ctime_tv_sec,
				new_entry->info.spoofed_atime_tv_nsec, new_entry->info.spoofed_mtime_tv_nsec, new_entry->info.spoofed_ctime_tv_nsec,
				new_entry->info.spoofed_blksize, new_entry->info.spoofed_blocks);
	} else {
		SUSFS_LOGI("is_statically: '%d', target_ino: '%lu', target_pathname: '%s', spoofed_ino: '%lu', spoofed_dev: '%lu', spoofed_nlink: '%u', spoofed_size: '%u', spoofed_atime_tv_sec: '%ld', spoofed_mtime_tv_sec: '%ld', spoofed_ctime_tv_sec: '%ld', spoofed_atime_tv_nsec: '%ld', spoofed_mtime_tv_nsec: '%ld', spoofed_ctime_tv_nsec: '%ld', spoofed_blksize: '%lu', spoofed_blocks: '%llu', is successfully updated to SUS_KSTAT_HLIST\n",
				new_entry->info.is_statically, new_entry->info.target_ino, new_entry->info.target_pathname,
				new_entry->info.spoofed_ino, new_entry->info.spoofed_dev,
				new_entry->info.spoofed_nlink, new_entry->info.spoofed_size,
				new_entry->info.spoofed_atime_tv_sec, new_entry->info.spoofed_mtime_tv_sec, new_entry->info.spoofed_ctime_tv_sec,
				new_entry->info.spoofed_atime_tv_nsec, new_entry->info.spoofed_mtime_tv_nsec, new_entry->info.spoofed_ctime_tv_nsec,
				new_entry->info.spoofed_blksize, new_entry->info.spoofed_blocks);
	}
#endif
	spin_unlock(&susfs_spin_lock_sus_kstat);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_sus_kstat __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	if (!info.is_statically) {
		SUSFS_LOGI("CMD_SUSFS_ADD_SUS_KSTAT -> ret: %d\n", info.err);
	} else {
		SUSFS_LOGI("CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY -> ret: %d\n", info.err);
	}
}

void susfs_update_sus_kstat(void __user **user_info) {
	struct st_susfs_sus_kstat info = {0};
	struct st_susfs_sus_kstat_hlist *new_entry, *tmp_entry;
	struct hlist_node *tmp_node;
	int bkt;

	if (copy_from_user(&info, (struct st_susfs_sus_kstat __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_sus_kstat);
	hash_for_each_safe(SUS_KSTAT_HLIST, bkt, tmp_node, tmp_entry, node) {
		if (!strcmp(tmp_entry->info.target_pathname, info.target_pathname)) {
			if (susfs_update_sus_kstat_inode(tmp_entry->info.target_pathname)) {
				info.err = -EINVAL;
				goto out_spin_unlock;
			}
			new_entry = kmalloc(sizeof(struct st_susfs_sus_kstat_hlist), GFP_KERNEL);
			if (!new_entry) {
				info.err = -ENOMEM;
				goto out_spin_unlock;
			}
			memcpy(&new_entry->info, &tmp_entry->info, sizeof(tmp_entry->info));
			SUSFS_LOGI("updating target_ino from '%lu' to '%lu' for pathname: '%s' in SUS_KSTAT_HLIST\n",
							new_entry->info.target_ino, info.target_ino, info.target_pathname);
			new_entry->target_ino = info.target_ino;
			new_entry->info.target_ino = info.target_ino;
			if (info.spoofed_size > 0) {
				SUSFS_LOGI("updating spoofed_size from '%lld' to '%lld' for pathname: '%s' in SUS_KSTAT_HLIST\n",
								new_entry->info.spoofed_size, info.spoofed_size, info.target_pathname);
				new_entry->info.spoofed_size = info.spoofed_size;
			}
			if (info.spoofed_blocks > 0) {
				SUSFS_LOGI("updating spoofed_blocks from '%llu' to '%llu' for pathname: '%s' in SUS_KSTAT_HLIST\n",
								new_entry->info.spoofed_blocks, info.spoofed_blocks, info.target_pathname);
				new_entry->info.spoofed_blocks = info.spoofed_blocks;
			}
			hash_del(&tmp_entry->node);
			kfree(tmp_entry);
			hash_add(SUS_KSTAT_HLIST, &new_entry->node, info.target_ino);
			info.err = 0;
			goto out_spin_unlock;
		}
	}
out_spin_unlock:
	spin_unlock(&susfs_spin_lock_sus_kstat);
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_sus_kstat __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_UPDATE_SUS_KSTAT -> ret: %d\n", info.err);
}

void susfs_sus_ino_for_generic_fillattr(unsigned long ino, struct kstat *stat) {
	struct st_susfs_sus_kstat_hlist *entry;

	hash_for_each_possible(SUS_KSTAT_HLIST, entry, node, ino) {
		if (entry->target_ino == ino) {
			stat->dev = entry->info.spoofed_dev;
			stat->ino = entry->info.spoofed_ino;
			stat->nlink = entry->info.spoofed_nlink;
			stat->size = entry->info.spoofed_size;
			stat->atime.tv_sec = entry->info.spoofed_atime_tv_sec;
			stat->atime.tv_nsec = entry->info.spoofed_atime_tv_nsec;
			stat->mtime.tv_sec = entry->info.spoofed_mtime_tv_sec;
			stat->mtime.tv_nsec = entry->info.spoofed_mtime_tv_nsec;
			stat->ctime.tv_sec = entry->info.spoofed_ctime_tv_sec;
			stat->ctime.tv_nsec = entry->info.spoofed_ctime_tv_nsec;
			stat->blocks = entry->info.spoofed_blocks;
			stat->blksize = entry->info.spoofed_blksize;
			return;
		}
	}
}

void susfs_sus_ino_for_show_map_vma(unsigned long ino, dev_t *out_dev, unsigned long *out_ino) {
	struct st_susfs_sus_kstat_hlist *entry;

	hash_for_each_possible(SUS_KSTAT_HLIST, entry, node, ino) {
		if (entry->target_ino == ino) {
			*out_dev = entry->info.spoofed_dev;
			*out_ino = entry->info.spoofed_ino;
			return;
		}
	}
}
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_KSTAT

/* try_umount */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
static DEFINE_SPINLOCK(susfs_spin_lock_try_umount);
extern void try_umount(const char *mnt, bool check_mnt, int flags);
static LIST_HEAD(LH_TRY_UMOUNT_PATH);
void susfs_add_try_umount(void __user **user_info) {
	struct st_susfs_try_umount info = {0};
	struct st_susfs_try_umount_list *new_list = NULL;

	if (copy_from_user(&info, (struct st_susfs_try_umount __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	if (info.mnt_mode == TRY_UMOUNT_DEFAULT) {
		info.mnt_mode = 0;
	} else if (info.mnt_mode == TRY_UMOUNT_DETACH) {
		info.mnt_mode = MNT_DETACH;
	} else {
		SUSFS_LOGE("Unsupported mnt_mode: %d\n", info.mnt_mode);
		info.err = -EINVAL;
		goto out_copy_to_user;
	}

	new_list = kmalloc(sizeof(struct st_susfs_try_umount_list), GFP_KERNEL);
	if (!new_list) {
		info.err = -ENOMEM;
		goto out_copy_to_user;
	}

	memcpy(&new_list->info, &info, sizeof(info));

	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock_try_umount);
	list_add_tail(&new_list->list, &LH_TRY_UMOUNT_PATH);
	spin_unlock(&susfs_spin_lock_try_umount);
	SUSFS_LOGI("target_pathname: '%s', umount options: %d, is successfully added to LH_TRY_UMOUNT_PATH\n", new_list->info.target_pathname, new_list->info.mnt_mode);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_try_umount __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ADD_TRY_UMOUNT -> ret: %d\n", info.err);
}

void susfs_try_umount(void) {
	struct st_susfs_try_umount_list *cursor = NULL;

	// We should umount in reversed order
	list_for_each_entry_reverse(cursor, &LH_TRY_UMOUNT_PATH, list) {
		try_umount(cursor->info.target_pathname, false, cursor->info.mnt_mode);
	}
}

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
void susfs_auto_add_try_umount_for_bind_mount(struct path *path) {
	struct st_susfs_try_umount_list *new_list = NULL;
	char *pathname = NULL, *dpath = NULL;
	size_t new_pathname_len = 0;

#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
	if (path->dentry->d_inode->i_mapping->flags & BIT_SUS_KSTAT) {
		SUSFS_LOGI("skip adding path to try_umount list as its inode is flagged INODE_STATE_SUS_KSTAT already\n");
		return;
	}
#endif

	pathname = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!pathname) {
		SUSFS_LOGE("no enough memory\n");
		return;
	}

	dpath = d_path(path, pathname, PAGE_SIZE);
	if (!dpath) {
		SUSFS_LOGE("dpath is NULL\n");
		goto out_free_pathname;
	}

	// - Important to check if it is from a magic mount, if so, then we need only
	//   the path which is directory only, others should be skipped.
	// - We need to strip out "/debug_ramdisk/workdir" here since there will be
	//   no "/debug_ramdisk/workdir" prefixed in zygote mnt ns
	if (!strncmp(dpath, "/debug_ramdisk/workdir/", 23)) {
		if (path->dentry->d_inode && S_ISDIR(path->dentry->d_inode->i_mode)) {
			new_pathname_len = strlen(dpath) - 22;
			memmove(dpath, dpath+22, new_pathname_len);
			*(dpath + new_pathname_len) = '\0';
			goto add_to_new_list;
		}
		goto out_free_pathname;
	}
add_to_new_list:
	new_list = kmalloc(sizeof(struct st_susfs_try_umount_list), GFP_KERNEL);
	if (!new_list) {
		SUSFS_LOGE("no enough memory\n");
		goto out_free_pathname;
	}

	strncpy(new_list->info.target_pathname, dpath, SUSFS_MAX_LEN_PATHNAME-1);

	new_list->info.mnt_mode = MNT_DETACH;

	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock_try_umount);
	list_add_tail(&new_list->list, &LH_TRY_UMOUNT_PATH);
	spin_unlock(&susfs_spin_lock_try_umount);
	SUSFS_LOGI("target_pathname: '%s', ino: %lu, umount options: %d, is successfully added to LH_TRY_UMOUNT_PATH\n",
					new_list->info.target_pathname, path->dentry->d_inode->i_ino, new_list->info.mnt_mode);
out_free_pathname:
	kfree(pathname);
}
#endif // #ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
#endif // #ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT

/* spoof_uname */
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
static DEFINE_SPINLOCK(susfs_spin_lock_set_uname);
static struct st_susfs_uname my_uname;
static void susfs_my_uname_init(void) {
	memset(&my_uname, 0, sizeof(my_uname));
}

void susfs_set_uname(void __user **user_info) {
	struct st_susfs_uname info = {0};

	if (copy_from_user(&info, (struct st_susfs_uname __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_set_uname);
	if (!strcmp(info.release, "default")) {
		strncpy(my_uname.release, utsname()->release, __NEW_UTS_LEN);
	} else {
		strncpy(my_uname.release, info.release, __NEW_UTS_LEN);
	}
	if (!strcmp(info.version, "default")) {
		strncpy(my_uname.version, utsname()->version, __NEW_UTS_LEN);
	} else {
		strncpy(my_uname.version, info.version, __NEW_UTS_LEN);
	}
	spin_unlock(&susfs_spin_lock_set_uname);
	SUSFS_LOGI("setting spoofed release: '%s', version: '%s'\n",
				my_uname.release, my_uname.version);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_uname __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SET_UNAME -> ret: %d\n", info.err);
}

void susfs_spoof_uname(struct new_utsname* tmp) {
	if (unlikely(my_uname.release[0] == '\0' || spin_is_locked(&susfs_spin_lock_set_uname)))
		return;
	strncpy(tmp->release, my_uname.release, __NEW_UTS_LEN);
	strncpy(tmp->version, my_uname.version, __NEW_UTS_LEN);
}
#endif // #ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME

/* enable_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
static DEFINE_SPINLOCK(susfs_spin_lock_enable_log);

void susfs_enable_log(void __user **user_info) {
	struct st_susfs_log info = {0};

	if (copy_from_user(&info, (struct st_susfs_log __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_enable_log);
	susfs_is_log_enabled = info.enabled;
	spin_unlock(&susfs_spin_lock_enable_log);
	if (susfs_is_log_enabled) {
		pr_info("susfs: enable logging to kernel");
	} else {
		pr_info("susfs: disable logging to kernel");
	}
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_log __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ENABLE_LOG -> ret: %d\n", info.err);
}
#endif // #ifdef CONFIG_KSU_SUSFS_ENABLE_LOG

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
static DEFINE_SPINLOCK(susfs_spin_lock_set_cmdline_or_bootconfig);
static char *fake_cmdline_or_bootconfig = NULL;
static bool susfs_is_fake_cmdline_or_bootconfig_set = false;

void susfs_set_cmdline_or_bootconfig(void __user **user_info) {
	struct st_susfs_spoof_cmdline_or_bootconfig *info = (struct st_susfs_spoof_cmdline_or_bootconfig *)kzalloc(sizeof(struct st_susfs_spoof_cmdline_or_bootconfig), GFP_KERNEL);
	
	if (!info) {
		info->err = -ENOMEM;
		goto out_copy_to_user;
	}

	if (copy_from_user(info, (struct st_susfs_spoof_cmdline_or_bootconfig __user*)*user_info, sizeof(struct st_susfs_spoof_cmdline_or_bootconfig))) {
		info->err = -EFAULT;
		goto out_copy_to_user;
	}

	if (!fake_cmdline_or_bootconfig) {
		fake_cmdline_or_bootconfig = (char *)kzalloc(SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE, GFP_KERNEL);
		if (!fake_cmdline_or_bootconfig) {
			info->err = -ENOMEM;
			goto out_copy_to_user;
		}
	}

	spin_lock(&susfs_spin_lock_set_cmdline_or_bootconfig);
	strncpy(fake_cmdline_or_bootconfig,
			info->fake_cmdline_or_bootconfig,
			SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE-1);
	spin_unlock(&susfs_spin_lock_set_cmdline_or_bootconfig);
	susfs_is_fake_cmdline_or_bootconfig_set = true;
	SUSFS_LOGI("fake_cmdline_or_bootconfig is set\n");
	info->err = 0;
out_copy_to_user:
	if (info->err) {
		susfs_is_fake_cmdline_or_bootconfig_set = false;
	}
	if (copy_to_user(&((struct st_susfs_spoof_cmdline_or_bootconfig __user*)*user_info)->err, &info->err, sizeof(info->err))) {
		info->err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> ret: %d\n", info->err);
	if (info) {
		kfree(info);
	}
}

int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m) {
	if (susfs_is_fake_cmdline_or_bootconfig_set && fake_cmdline_or_bootconfig) {
		seq_puts(m, fake_cmdline_or_bootconfig);
		return 0;
	}
	return 1;
}
#endif

/* open_redirect */
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
static DEFINE_SPINLOCK(susfs_spin_lock_open_redirect);
static DEFINE_HASHTABLE(OPEN_REDIRECT_HLIST, 10);

static int susfs_update_open_redirect_inode(struct st_susfs_open_redirect_hlist *new_entry) {
	struct path path_target;
	struct inode *inode_target;
	int err = 0;

	err = kern_path(new_entry->target_pathname, LOOKUP_FOLLOW, &path_target);
	if (err) {
		SUSFS_LOGE("Failed opening file '%s'\n", new_entry->target_pathname);
		return err;
	}

	inode_target = d_inode(path_target.dentry);
	if (!inode_target) {
		SUSFS_LOGE("inode_target is NULL\n");
		err = -EINVAL;
		goto out_path_put_target;
	}

	spin_lock(&inode_target->i_lock);
	set_bit(AS_FLAGS_OPEN_REDIRECT, &inode_target->i_mapping->flags);
	spin_unlock(&inode_target->i_lock);

out_path_put_target:
	path_put(&path_target);
	return err;
}

void susfs_add_open_redirect(void __user **user_info) {
	struct st_susfs_open_redirect info = {0};
	struct st_susfs_open_redirect_hlist *new_entry, *tmp_entry;
	struct hlist_node *tmp_node;
	int bkt;
	bool update_hlist = false;

	if (copy_from_user(&info, (struct st_susfs_open_redirect __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_open_redirect);
	hash_for_each_safe(OPEN_REDIRECT_HLIST, bkt, tmp_node, tmp_entry, node) {
		if (!strcmp(tmp_entry->target_pathname, info.target_pathname)) {
			hash_del(&tmp_entry->node);
			kfree(tmp_entry);
			update_hlist = true;
			break;
		}
	}
	spin_unlock(&susfs_spin_lock_open_redirect);

	new_entry = kmalloc(sizeof(struct st_susfs_open_redirect_hlist), GFP_KERNEL);
	if (!new_entry) {
		info.err = -ENOMEM;
		goto out_copy_to_user;
	}

	new_entry->target_ino = info.target_ino;
	strncpy(new_entry->target_pathname, info.target_pathname, SUSFS_MAX_LEN_PATHNAME-1);
	strncpy(new_entry->redirected_pathname, info.redirected_pathname, SUSFS_MAX_LEN_PATHNAME-1);
	if (susfs_update_open_redirect_inode(new_entry)) {
		SUSFS_LOGE("failed adding path '%s' to OPEN_REDIRECT_HLIST\n", new_entry->target_pathname);
		kfree(new_entry);
		info.err = -EINVAL;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_open_redirect);
	hash_add(OPEN_REDIRECT_HLIST, &new_entry->node, info.target_ino);
	if (update_hlist) {
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', redirected_pathname: '%s', is successfully updated to OPEN_REDIRECT_HLIST\n",
				new_entry->target_ino, new_entry->target_pathname, new_entry->redirected_pathname);	
	} else {
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s' redirected_pathname: '%s', is successfully added to OPEN_REDIRECT_HLIST\n",
				new_entry->target_ino, new_entry->target_pathname, new_entry->redirected_pathname);
	}
	spin_unlock(&susfs_spin_lock_open_redirect);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_open_redirect __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ADD_OPEN_REDIRECT -> ret: %d\n", info.err);
}

struct filename* susfs_get_redirected_path(unsigned long ino) {
	struct st_susfs_open_redirect_hlist *entry;

	hash_for_each_possible(OPEN_REDIRECT_HLIST, entry, node, ino) {
		if (entry->target_ino == ino) {
			SUSFS_LOGI("Redirect for ino: %lu\n", ino);
			return getname_kernel(entry->redirected_pathname);
		}
	}
	return ERR_PTR(-ENOENT);
}
#endif // #ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT

/* sus_map */
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
void susfs_add_sus_map(void __user **user_info) {
	struct st_susfs_sus_map info = {0};
	struct path path;
	struct inode *inode = NULL;
	if (copy_from_user(&info, (struct st_susfs_sus_map __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	info.err = kern_path(info.target_pathname, LOOKUP_FOLLOW, &path);
	if (info.err) {
		SUSFS_LOGE("Failed opening file '%s'\n", info.target_pathname);
		goto out_copy_to_user;
	}

	if (!path.dentry->d_inode) {
		info.err = -EINVAL;
		goto out_path_put_path;
	}
	inode = d_inode(path.dentry);
	spin_lock(&inode->i_lock);
	set_bit(AS_FLAGS_SUS_MAP, &inode->i_mapping->flags);
	SUSFS_LOGI("pathname: '%s', is flagged as AS_FLAGS_SUS_MAP\n", info.target_pathname);
	spin_unlock(&inode->i_lock);
	info.err = 0;
	out_path_put_path:
	path_put(&path);
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_sus_map __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ADD_SUS_MAP -> ret: %d\n", info.err);
}
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP

/* susfs avc log spoofing */
static DEFINE_SPINLOCK(susfs_spin_lock_set_avc_log_spoofing);
extern bool susfs_is_avc_log_spoofing_enabled;

void susfs_set_avc_log_spoofing(void __user **user_info) {
	struct st_susfs_avc_log_spoofing info = {0};

	if (copy_from_user(&info, (struct st_susfs_avc_log_spoofing __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_set_avc_log_spoofing);
	susfs_is_avc_log_spoofing_enabled = info.enabled;
	spin_unlock(&susfs_spin_lock_set_avc_log_spoofing);
	SUSFS_LOGI("susfs_is_avc_log_spoofing_enabled: %d\n", info.enabled);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_avc_log_spoofing __user*)*user_info)->err, &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING -> ret: %d\n", info.err);
}

/* get susfs enabled features */
static int copy_config_to_buf(const char *config_string, char *buf_ptr, size_t *copied_size, size_t bufsize) {
	size_t tmp_size = strlen(config_string);

	*copied_size += tmp_size;
	if (*copied_size >= bufsize) {
		SUSFS_LOGE("bufsize is not big enough to hold the string.\n");
		return -EINVAL;
	}
	strncpy(buf_ptr, config_string, tmp_size);
	return 0;
}

void susfs_get_enabled_features(void __user **user_info) {
	struct st_susfs_enabled_features *info = (struct st_susfs_enabled_features *)kzalloc(sizeof(struct st_susfs_enabled_features), GFP_KERNEL);
	char *buf_ptr = NULL;
	size_t copied_size = 0;

	if (!info) {
		info->err = -ENOMEM;
		goto out_copy_to_user;
	}

	if (copy_from_user(info, (struct st_susfs_enabled_features __user*)*user_info, sizeof(struct st_susfs_enabled_features))) {
		info->err = -EFAULT;
		goto out_copy_to_user;
	}

	buf_ptr = info->enabled_features;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_PATH\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_MOUNT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_KSTAT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_TRY_UMOUNT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SPOOF_UNAME\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_ENABLE_LOG\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_OPEN_REDIRECT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_MAP\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif

	info->err = 0;
out_copy_to_user:
	if (copy_to_user((struct st_susfs_enabled_features __user*)*user_info, info, sizeof(struct st_susfs_enabled_features))) {
		info->err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SHOW_ENABLED_FEATURES -> ret: %d\n", info->err);
	if (info) {
		kfree(info);
	}
}

/* show_variant */
void susfs_show_variant(void __user **user_info) {
	struct st_susfs_variant info = {0};

	if (copy_from_user(&info, (struct st_susfs_variant __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	strncpy(info.susfs_variant, SUSFS_VARIANT, SUSFS_MAX_VARIANT_BUFSIZE-1);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user((struct st_susfs_variant __user*)*user_info, &info, sizeof(info))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SHOW_VARIANT -> ret: %d\n", info.err);
}

/* show version */
void susfs_show_version(void __user **user_info) {
	struct st_susfs_version info = {0};

	if (copy_from_user(&info, (struct st_susfs_version __user*)*user_info, sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	strncpy(info.susfs_version, SUSFS_VERSION, SUSFS_MAX_VERSION_BUFSIZE-1);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user((struct st_susfs_version __user*)*user_info, &info, sizeof(info))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SHOW_VERSION -> ret: %d\n", info.err);
}

/* susfs_init */
void susfs_init(void) {
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	susfs_my_uname_init();
#endif
	SUSFS_LOGI("susfs is initialized! version: " SUSFS_VERSION " \n");
}

/* No module exit is needed becuase it should never be a loadable kernel module */
//void __init susfs_exit(void)

