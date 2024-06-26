// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2023-2024 Micron Technology, Inc.  All rights reserved.
 */

#include <gtest/gtest.h>

extern "C" {
#include <unistd.h>
#include <linux/limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include <linux/famfs_ioctl.h>
#include "famfs_lib.h"
#include "famfs_lib_internal.h"
#include "famfs_meta.h"
#include "xrand.h"
#include "random_buffer.h"
#include "famfs_unit.h"
}

/****+++++++++++++++++++++++++++++++++++++++++++++
 * NOTE THESE TESTS MUST BE RUN AS ROOT!!
 * (perhaps we'll get around to mitigaing this...)
 */

#define FAMFS_MPT "/mnt/famfs"
#define DIRPATH   "/mnt/famfs/testdir42"
#define TESTFILE  "/mnt/famfs/testdir42/testfile0"
#define PATH	  256
#define SYS_UUID_DIR "/opt/famfs"

TEST(famfs, dummy)
{
	printf("Dummy test\n");
	ASSERT_EQ(0, 0);
}

TEST(famfs, famfs_create_sys_uuid_file)
{
	char sys_uuid_file[PATH];
	int rc;
	extern int mock_uuid;
	uuid_le uuid_out;


	// Check with correct file name and path
	snprintf(sys_uuid_file, PATH, "/opt/famfs/system_uuid");
	rc = famfs_create_sys_uuid_file(sys_uuid_file);
	ASSERT_EQ(rc, 0);

	// Pass a directory, should fail
	system("mkdir -p /tmp/famfs");
	snprintf(sys_uuid_file, PATH, "%s", "/tmp/famfs");
	rc = famfs_create_sys_uuid_file(sys_uuid_file);
	ASSERT_NE(rc, 0);

	// create a uuid file
	snprintf(sys_uuid_file, PATH, "/tmp/system_uuid");
	rc = famfs_create_sys_uuid_file(sys_uuid_file);
	ASSERT_EQ(rc, 0);
	system("rm /tmp/system_uuid");

	// simulate directory creation failure
	mock_uuid = 1;
	system("mv /opt/famfs /opt/famfs_old");
	snprintf(sys_uuid_file, PATH, "/opt/famfs/system_uuid");
	rc = famfs_create_sys_uuid_file(sys_uuid_file);
	ASSERT_NE(rc, 0);
	system("rmdir /opt/famfs");
	system("mv /opt/famfs_old /opt/famfs");
	mock_uuid = 0;

	// simulate write failure with mock_uuid
	mock_uuid = 1;
	snprintf(sys_uuid_file, PATH, "/tmp/system_uuid");
	rc = famfs_create_sys_uuid_file(sys_uuid_file);
	ASSERT_NE(rc, 0);

	// simulate fscanf failure in famfs_get_system_uuid
	rc = famfs_get_system_uuid(&uuid_out);
	ASSERT_NE(rc, 0);
	mock_uuid = 0;

}

TEST(famfs, famfs_mkfs)
{
	u64 device_size = 1024 * 1024 * 1024;
	struct famfs_superblock *sb;
	struct famfs_log *logp;
	int rc;

	/* Prepare a fake famfs (move changes to this block everywhere it is) */
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);

	/* Repeat should fail because there is a valid superblock */
	rc = __famfs_mkfs("/dev/dax0.0", sb, logp, device_size, 0, 0);
	ASSERT_NE(rc, 0);

	/* Repeat with kill and force should succeed */
	rc = __famfs_mkfs("/dev/dax0.0", sb, logp, device_size, 1, 1);
	ASSERT_EQ(rc, 0);

	/* Repeat without force should succeed because we wiped out the old superblock */
	rc = __famfs_mkfs("/dev/dax0.0", sb, logp, device_size, 0, 0);
	ASSERT_EQ(rc, 0);

	/* Repeat without force should fail because there is a valid sb again */
	rc = __famfs_mkfs("/dev/dax0.0", sb, logp, device_size, 0, 0);
	ASSERT_NE(rc, 0);

	/* Repeat with force should succeed because of force */
	rc = __famfs_mkfs("/dev/dax0.0", sb, logp, device_size, 1, 0);
	ASSERT_EQ(rc, 0);

	/* This leaves a valid superblock and log at /tmp/famfs/.meta ... */

}

TEST(famfs, famfs_super_test)
{
	u64 device_size = 1024 * 1024 * 1024;
	struct famfs_superblock *sb = NULL;
	struct famfs_log *logp;
	extern int mock_flush;
	int rc;

	mock_flush = 1;

	/* null superblock should fail */
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, -1);

	sb = (struct famfs_superblock *)calloc(1, sizeof(*sb));
	logp = (struct famfs_log *)calloc(1, FAMFS_LOG_LEN);

	/* Make a fake file system with our fake sb and log */
	rc = __famfs_mkfs("/dev/dax0.0", sb, logp, device_size, 0, 0);
	ASSERT_EQ(rc, 0);

	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, 0);

	sb->ts_magic--; /* bad magic number */
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, -1);

	sb->ts_magic++; /* good magic number */
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, 0);

	sb->ts_version++;  /* unrecognized version */
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, -1);

	sb->ts_version = FAMFS_CURRENT_VERSION;  /* version good again */
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, 0);

	sb->ts_crc++; /* bad crc */
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, -1);

	sb->ts_crc = famfs_gen_superblock_crc(sb);
	rc = famfs_check_super(sb);
	ASSERT_EQ(rc, 0); /* good crc */

	logp->famfs_log_magic++;
	rc = famfs_validate_log_header(logp);
	ASSERT_LT(rc, 0);

	logp->famfs_log_magic--;
	logp-> famfs_log_crc++;
	rc = famfs_validate_log_header(logp);
	ASSERT_LT(rc, 0);

	logp->famfs_log_crc--;
	rc = famfs_validate_log_header(logp);
	ASSERT_EQ(rc, 0);
}

#define SB_RELPATH ".meta/.superblock"
#define LOG_RELPATH ".meta/.log"

TEST(famfs, famfs_open_relpath)
{
	int rc;

	/* TODO: add relative path checks (getcwd(), chdir(), use rellative paths, chdir back) */

	/* /tmp/famfs should already exist and have a superblock and log in it */
	system("mkdir -p /tmp/famfs/0000/1111/2222/3333/4444/5555");

	rc = __open_relpath("/tmp/bogus/path", SB_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_NE(rc, 0);;

	rc = __open_relpath("/tmp/bogus/path", SB_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_NE(rc, 0);

	/* Good, no ascent necessary  */
	rc = __open_relpath("/tmp/famfs/", LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_GT(rc, 0);
	close(rc);
	rc = __open_relpath("/tmp/famfs", LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_GT(rc, 0);
	close(rc);

	/* Good but deep path */
	rc = __open_relpath("/tmp/famfs/0000/1111/2222/3333/4444/5555",
			    LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_GT(rc, 0);
	close(rc);

	/* Bogus path that ascends to a real path with .meta */
	rc = __open_relpath("/tmp/famfs/0000/1111/2222/3333/4444/5555/66666",
			    LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_GT(rc, 0);
	close(rc);

	/* Deep bogus path that ascends to a real path with .meta */
	rc = __open_relpath("/tmp/famfs/0000/1111/2222/3333/4444/5555/66666/7/6/5/4/3/2/xxx",
			    LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_GT(rc, 0);
	close(rc);

	/* empty path */
	rc = __open_relpath("", LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_LT(rc, 0);
	close(rc);

	/* "/" */
	rc = __open_relpath("/", LOG_RELPATH, 1, NULL, NULL, NO_LOCK, 1);
	ASSERT_LT(rc, 0);
	close(rc);

	/* No "/" */
	rc = __open_relpath("blablabla", LOG_RELPATH, 1, NULL, NULL, BLOCKING_LOCK, 1);
	ASSERT_LT(rc, 0);
	close(rc);
	/* No "/" and spaces */
	rc = __open_relpath("bla bla bla", LOG_RELPATH, 1, NULL, NULL, NON_BLOCKING_LOCK, 1);
	ASSERT_LT(rc, 0);
	close(rc);
}

TEST(famfs, famfs_get_device_size)
{
	enum famfs_extent_type type;
	size_t size;
	int rc;

	rc = famfs_get_device_size("/dev/zero", &size, &type);
	ASSERT_NE(rc, 0);
	rc = famfs_get_device_size("badfile", &size, &type);
	ASSERT_NE(rc, 0);
	rc = famfs_get_device_size("/etc/hosts", &size, &type);
	ASSERT_NE(rc, 0);
}

TEST(famfs, famfs_xrand64_tls)
{
	u_int64_t num;
	struct xrand xr;

	xrand_init(&xr, 42);
	ASSERT_NE(num, 0);
	num = xrand64_tls();
	ASSERT_NE(num, 0);
	num = xrand_range64(&xr, 42, 0x100000);
	ASSERT_NE(num, 0);
}

TEST(famfs, famfs_random_buffer)
{
	struct xrand xr;
	//u_int32_t rnum;
	char buf[16];
	int rc;

	xrand_init(&xr, 42);
	randomize_buffer(buf, 0, 11);
	rc = validate_random_buffer(buf, 0, 11);
	ASSERT_EQ(rc, -1);
#if 0
	rnum = generate_random_u32(1, 10);
	ASSERT_GT(rnum, 0);
	ASSERT_LT(rnum, 11);
#endif
}

#define booboofile "/tmp/booboo"
TEST(famfs, famfs_file_not_famfs)
{
	int sfd;
	int rc;
	extern int mock_kmod;
	int mock_kmod_save = mock_kmod;

	system("rm -rf " booboofile);
	sfd = open(booboofile, O_RDWR | O_CREAT, 0666);
	ASSERT_NE(sfd, 0);

	mock_kmod = 0;
	rc = __file_not_famfs(sfd);
	ASSERT_NE(rc, 0);
	mock_kmod = mock_kmod_save;
	close(sfd);

	rc = file_not_famfs(booboofile);
	ASSERT_NE(rc, 0);

	rc = file_not_famfs("/tmp/non-existent-file");
	ASSERT_LT(rc, 0);
}

TEST(famfs, famfs_mkmeta)
{
	int rc;

	rc = famfs_mkmeta("/dev/bogusdev");
	ASSERT_NE(rc, 0);
}

TEST(famfs, mmap_whole_file)
{
	size_t size;
	void *addr;
	int sfd;

	addr = famfs_mmap_whole_file("bogusfile", 1, &size);
	ASSERT_NE(addr, MAP_FAILED);
	addr = famfs_mmap_whole_file("/dev/zero", 1, &size);
	ASSERT_NE(addr, MAP_FAILED);

	sfd = open("/tmp/famfs/frab", O_RDWR | O_CREAT, 0666); /* make empty file */
	ASSERT_GT(sfd, 0);
	close(sfd);
	addr = famfs_mmap_whole_file("/tmp/famfs/frab", 1, 0); /* empty file */
	ASSERT_EQ((long long)addr, 0);
}

TEST(famfs, __famfs_cp)
{
	int rc;
	u64 device_size = 1024 * 1024 * 256;
	struct famfs_locked_log ll;
	struct famfs_superblock *sb;
	struct famfs_log *logp;
	extern int mock_failure;
	extern int mock_kmod;

	/* Prepare a fake famfs  */
	mock_kmod = 1;
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);
	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_EQ(rc, 0);
	mock_kmod = 0;

	/* OK, this is coverage hackery. Beware */
	rc = __famfs_cp((struct famfs_locked_log *)0xdeadbeef,
			"badsrcfile",
			"xx",
			0, 0, 0, 0);
	ASSERT_EQ(rc, 1);
	rc = __famfs_cp((struct famfs_locked_log *)0xdeadbeef,
			"/etc",
			"xx",
			0, 0, 0, 0);
	ASSERT_EQ(rc, 1);
	rc = __famfs_cp((struct famfs_locked_log *)0xdeadbeef,
			"/dev/zero",
			"xx",
			0, 0, 0, 0);
	ASSERT_EQ(rc, 1);


	/* exercise verbose path */
	system("touch /tmp/src");
	rc = __famfs_cp((struct famfs_locked_log *)0xdeadbeef,
			"/tmp/src",
			"xx",
			0, 0, 0, 2);
	ASSERT_EQ(rc, 1);
	system("rm /tmp/src");

	/* fail open of src file */
	system("dd if=/dev/random of=/tmp/src bs=4096 count=1");
	mock_failure = MOCK_FAIL_OPEN;
	rc = __famfs_cp((struct famfs_locked_log *)0xdeadbeef,
			"/tmp/src",
			"xx",
			0, 0, 0, 2);
	ASSERT_EQ(rc, 1);
	mock_failure = MOCK_FAIL_NONE;
	system("rm /tmp/src");

	/* fail fd of dest file */
	system("dd if=/dev/random of=/tmp/src bs=4096 count=1");
	rc = __famfs_cp((struct famfs_locked_log *)0xdeadbeef,
			"/tmp/src",
			"/tmp/dest",
			0, 0, 0, 2);
	system("rm /tmp/src");
	ASSERT_NE(rc, 0);

	/* fail mmap of dest file*/
	system("dd if=/dev/random of=/tmp/src bs=4096 count=1");
	mock_kmod = 1;
	mock_failure = MOCK_FAIL_MMAP;
	rc = __famfs_cp(&ll,
			"/tmp/src",
			"/tmp/famfs/dest",
			0, 0, 0, 2);
	system("rm /tmp/src");
	mock_failure = MOCK_FAIL_NONE;
	mock_kmod = 0;
	ASSERT_NE(rc, 0);

	/* fail srcfile read */
	system("dd if=/dev/random of=/tmp/src bs=4096 count=1");
	mock_kmod = 1;
	rc = __famfs_cp(&ll,
			"/tmp/src",
			"/tmp/famfs/dest",
			0, 0, 0, 2);
	system("rm /tmp/src");
	mock_kmod = 0;
	ASSERT_NE(rc, 0);
}

TEST(famfs, famfs_log)
{
	u64 device_size = 1024 * 1024 * 1024;
	struct famfs_locked_log ll;
	struct famfs_superblock *sb;
	struct famfs_log *logp;
	extern int mock_kmod;
	extern int mock_role;
	extern int mock_path;
	extern int mock_failure;
	int rc;
	int i;
	u64 tmp;

	mock_kmod = 1;
	/** can call famfs_file_alloc() and __famfs_mkdir() on our fake famfs in /tmp/famfs */

	/* Prepare a fake famfs (move changes to this block everywhere it is) */
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);

	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_EQ(rc, 0);

	for (i = 0; i < 503; i++) {
		char filename[64];
		int fd;
		sprintf(filename, "/tmp/famfs/%04d", i);
		fd = __famfs_mkfile(&ll, filename, 0, 0, 0, 1048576, 0);
		if (i < 502)
			ASSERT_GT(fd, 0);
		else
			ASSERT_LT(fd, 0); /* out of space */
		close(fd);
	}

	for (i = 0; i < 100; i++) {
		char dirname[64];
		sprintf(dirname, "/tmp/famfs/dir%04d", i);
		rc = __famfs_mkdir(&ll, dirname, 0, 0, 0, 0);
		ASSERT_EQ(rc, 0);
	}
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 3);
	ASSERT_EQ(rc, 0);

	/* fail sb sanity check */
	rc = __famfs_logplay(logp, "/tmp/famfs1", 0, 0, 4);
	ASSERT_NE(rc, 0);

	/* fail famfs_check_super */
	sb->ts_magic = 420;
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 4);
	ASSERT_NE(rc, 0);
	sb->ts_magic = FAMFS_SUPER_MAGIC;

	/* fail FAMFS_LOG_MAGIC check */
	logp->famfs_log_magic = 420;
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 4);
	ASSERT_NE(rc, 0);
	logp->famfs_log_magic = FAMFS_LOG_MAGIC;

	/* fail famfs_validate_log_entry() */
	tmp = logp->entries[0].famfs_log_entry_seqnum;
	logp->entries[0].famfs_log_entry_seqnum = 420;
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 4);
	ASSERT_NE(rc, 0);
	logp->entries[0].famfs_log_entry_seqnum = tmp;

	/* fail famfs_log_entry_fc_path_is_relative */
	mock_path = 1;
	tmp = logp->entries[0].famfs_log_entry_type;
	logp->entries[0].famfs_log_entry_type = FAMFS_LOG_FILE;
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 0);
	ASSERT_NE(rc, 0);
	mock_path = 0;
	logp->entries[0].famfs_log_entry_type = tmp;

	/* reach FAMFS_LOG_ACCESS */
	mock_failure = MOCK_FAIL_GENERIC;
	tmp = logp->entries[0].famfs_log_entry_type;
	logp->entries[0].famfs_log_entry_type = FAMFS_LOG_ACCESS;
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 1);
	ASSERT_EQ(rc, 0);
	mock_failure = MOCK_FAIL_NONE;
	logp->entries[0].famfs_log_entry_type = tmp;


	/* fail famfs_log_entry_md_path_is_relative for FAMFS_LOG_MKDIR */
	mock_failure = MOCK_FAIL_LOG_MKDIR;
	tmp = logp->entries[0].famfs_log_entry_type;
	logp->entries[0].famfs_log_entry_type = FAMFS_LOG_MKDIR;
	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 0);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;
	logp->entries[0].famfs_log_entry_type = tmp;

	rc = famfs_fsck_scan(sb, logp, 1, 3);
	ASSERT_EQ(rc, 0);

	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 1 /* mmap */, 1, 1);
	ASSERT_EQ(rc, 0);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_EQ(rc, 0);
	rc = famfs_fsck("/tmp/nonexistent-file", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);

	/* Save good copies of the log and superblock */
	system("cp /tmp/famfs/.meta/.log /tmp/famfs/.meta/.log.save");
	system("cp /tmp/famfs/.meta/.superblock /tmp/famfs/.meta/.superblock.save");

	truncate("/tmp/famfs/.meta/.superblock", 8192);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_EQ(rc, 0);

	truncate("/tmp/famfs/.meta/.superblock", 7);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);

	truncate("/tmp/famfs/.meta/.log", 8192);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);

	unlink("/tmp/famfs/.meta/.log");
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 1 /* mmap */, 1, 1);
	ASSERT_NE(rc, 0);
	unlink("/tmp/famfs/.meta/.superblock");
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 1 /* mmap */, 1, 1);
	ASSERT_NE(rc, 0);

	system("chmod 200 /tmp/famfs/.meta/.log");
	rc = famfs_fsck("/tmp/famfs/.meta/.log", 1 /* mmap */, 1, 1);
	ASSERT_NE(rc, 0);
	rc = famfs_fsck("/tmp/famfs/.meta/.log", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);

	system("chmod 200 /tmp/famfs/.meta/.superblock");
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 1 /* mmap */, 1, 1);
	ASSERT_NE(rc, 0);
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);

	system("cp /tmp/famfs/.meta/.log.save /tmp/famfs/.meta/.log");
	system("cp /tmp/famfs/.meta/.superblock.save /tmp/famfs/.meta/.superblock");

	rc = famfs_release_locked_log(&ll);
	ASSERT_EQ(rc, 0);

	system("chmod 444 /tmp/famfs/.meta/.log"); /* log file not writable */

	mock_role = FAMFS_CLIENT;
	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_NE(rc, 0);

	mock_role = FAMFS_CLIENT;
	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_NE(rc, 0); /* init_locked_log should fail as client */

	mock_role = 0;

	mock_failure = MOCK_FAIL_OPEN_SB;
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	mock_failure = MOCK_FAIL_READ_SB;
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	mock_failure = MOCK_FAIL_OPEN_LOG;
	rc = famfs_fsck("/tmp/famfs/.meta/.log", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	mock_failure = MOCK_FAIL_READ_LOG;
	rc = famfs_fsck("/tmp/famfs/.meta/.log", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	mock_failure = MOCK_FAIL_READ_FULL_LOG;
	rc = famfs_fsck("/tmp/famfs/.meta/.log", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	/* create a invalide block device to fail _get_Device_size*/
	system("mknod -m 200 /tmp/testblock b 3 3");
	rc = famfs_fsck("/tmp/testblock", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	system("rm /tmp/testblock");

	/* create a non-reg, non-block, non char device, i.e. pipe device*/
	system("mknod -m 200 /tmp/testpipe p");
	rc = famfs_fsck("/tmp/testpipe", 0 /* read */, 1, 1);
	ASSERT_NE(rc, 0);
	system("rm /tmp/testpipe");

#if 0
	/* this stuff is not working as expected. leaving for now. */
	printf("uuid before: ");
	famfs_print_uuid(&sb->ts_system_uuid);
	/* change sys uuid in superblock (crc now wrong) */
	memset(&sb->ts_system_uuid, 0, sizeof(uuid_le));

	printf("uuid after: ");
	famfs_print_uuid(&sb->ts_system_uuid);

	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_NE(rc, 0);

	/* now fix crc */
	sb->ts_crc = famfs_gen_superblock_crc(sb);
	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_NE(rc, 0);
#endif

}

TEST(famfs, famfs_log_overflow_mkdir_p)
{
	u64 device_size = 64ULL * 1024ULL * 1024ULL * 1024ULL;
	//struct famfs_locked_log ll;
	struct famfs_superblock *sb;
	char dirname[PATH_MAX];
	struct famfs_log *logp;
	extern int mock_kmod;
	int rc;
	int i;

	mock_kmod = 1;
	/** can call famfs_file_alloc() and __famfs_mkdir() on our fake famfs in /tmp/famfs */

	/* Prepare a fake famfs (move changes to this block everywhere it is) */
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);

	/* TODO: nested dirs and files to fill up the log */
	for (i = 0; ; i++) {
		s64 nslots = log_slots_available(logp);

		sprintf(dirname, "/tmp/famfs/dir%04d/a/b/c/d/e/f/g/h/i", i);
		/* mkdir -p */
		rc = famfs_mkdir_parents(dirname, 0644, 0, 0, (i < 2500) ? 0:2);

		if (nslots >= 10) {
			if (rc != 0)
				printf("nslots: %lld\n", nslots);
			ASSERT_EQ(rc, 0);
		} else {
			printf("nslots: %lld\n", nslots);
			ASSERT_NE(rc, 0);
			break;
		}
	}

	/* Let's check how many log entries are left */
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_EQ(rc, 0);

	famfs_dump_log(logp);

	/* Let's check how many log entries are left */
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_EQ(rc, 0);

	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 0);
	ASSERT_EQ(rc, 0);
	//famfs_print_log_stats("famfs_log test", )

	rc = famfs_fsck_scan(sb, logp, 1, 0);
	ASSERT_EQ(rc, 0);
}

TEST(famfs, famfs_clone) {
	u64 device_size = 1024 * 1024 * 256;
	struct famfs_locked_log ll;
	struct famfs_superblock *sb;
	struct famfs_log *logp;
	extern int mock_failure;
	extern int mock_role;
	extern int mock_kmod;
	char filename[PATH_MAX * 2];
	int fd;
	int rc = 0;

	/* Prepare a fake famfs  */
	mock_kmod = 1;
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);
	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_EQ(rc, 0);
	sprintf(filename, "/tmp/famfs/clonesrc");
	fd = __famfs_mkfile(&ll, filename, 0, 0, 0, 2097152, 1);
	ASSERT_GT(fd, 0);
	mock_kmod = 0;

	/* clone a nonexistant srcfile and fail*/
	rc = famfs_clone("/tmp/nonexistant", "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);

	/* clone existing file but not in famfs and fail */
	system("touch /tmp/randfile");
	rc = famfs_clone("/tmp/randfile", "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);

	mock_kmod = 1; /* this is needed to show srcfile as in fake famfs */
	/* fail to stat srcfile */
	mock_failure = MOCK_FAIL_GENERIC;
	rc = famfs_clone(filename, "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	/* fail to check role srcfile */
	mock_failure = MOCK_FAIL_SROLE;
	rc = famfs_clone(filename, "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	/* fail to check role destfile */
	rc = famfs_clone(filename, "/tmp/famfs1/f1", 1);
	ASSERT_NE(rc, 0);

	/* fail to check srcfile and destfile in same FS */
	mock_failure = MOCK_FAIL_ROLE;
	rc = famfs_clone(filename, "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	/* fail to create file in client role */
	mock_role = FAMFS_CLIENT;
	rc = famfs_clone(filename, "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);
	mock_role = 0;

	/* fail to open srcfile */
	mock_failure = MOCK_FAIL_OPEN;
	rc = famfs_clone(filename, "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	/* fail to do MAP_GET ioctl */
	rc = famfs_clone(filename, "/tmp/famfs/f1", 1);
	ASSERT_NE(rc, 0);
}

TEST(famfs, famfs_log_overflow_files)
{
	u64 device_size = 64ULL * 1024ULL * 1024ULL * 1024ULL;
	struct famfs_superblock *sb;
	char dirname[PATH_MAX];
	char filename[PATH_MAX * 2];
	struct famfs_log *logp;
	extern int mock_kmod;
	int fd;
	int rc;
	int i;

	mock_kmod = 1;
	/** can call famfs_file_alloc() and __famfs_mkdir() on our fake famfs in /tmp/famfs */

	/* Prepare a fake famfs (move changes to this block everywhere it is) */
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);

	/* Keep doing "mkdir -p" until the log is almost full.
	 * Each of these commands will use 10 log entries.
	 */
	for (i = 0; ; i++) {
		sprintf(dirname, "/tmp/famfs/dir%04d/a/b/c/d/e/f/g/h/i", i);
		/* mkdir -p */
		rc = famfs_mkdir_parents(dirname, 0644, 0, 0, (i < 2500) ? 0:2);
		ASSERT_EQ(rc, 0);

		sprintf(filename, "%s/%04d", dirname, i);
		fd = famfs_mkfile(filename, 0, 0, 0, 1048576, 0);
		ASSERT_GT(fd, 0);

		close(fd);

		/* When we're close to full, break and create files */
		if (log_slots_available(logp) < 12)
			break;
	}

	for (i = 0 ; ; i++) {
		printf("xyi: %d\n", i);
		sprintf(filename, "%s/%04d", dirname, i);
		fd = famfs_mkfile(filename, 0, 0, 0, 1048576, 0);
		if (log_slots_available(logp) > 0) {
			ASSERT_GT(fd, 0);
			close(fd);
		} else if (log_slots_available(logp) == 0) {
			fd = famfs_mkfile(filename, 0, 0, 0, 1048576, 0);
			ASSERT_LT(fd, 0);
			break;
		}
	}

	/* Let's check how many log entries are left */
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_EQ(rc, 0);

	famfs_dump_log(logp);

	/* Let's check how many log entries are left */
	rc = famfs_fsck("/tmp/famfs/.meta/.superblock", 0 /* read */, 1, 1);
	ASSERT_EQ(rc, 0);

	rc = __famfs_logplay(logp, "/tmp/famfs", 0, 0, 0);
	ASSERT_EQ(rc, 0);

	rc = famfs_fsck_scan(sb, logp, 1, 3);
	ASSERT_EQ(rc, 0);
}

TEST(famfs, famfs_cp) {
	u64 device_size = 1024 * 1024 * 256;
	struct famfs_locked_log ll;
	struct famfs_superblock *sb;
	struct famfs_log *logp;
	extern int mock_failure;
	extern int mock_kmod;
	char src[PATH_MAX * 2];
	char dest[PATH_MAX * 2];
	int rc = 0;

	/* Prepare a fake famfs  */
	mock_kmod = 1;
	rc = create_mock_famfs_instance("/tmp/famfs", device_size, &sb, &logp);
	ASSERT_EQ(rc, 0);
	rc = famfs_init_locked_log(&ll, "/tmp/famfs", 1);
	ASSERT_EQ(rc, 0);
	mock_kmod = 0;

	system("mkdir -p /tmp/destdir");
	sprintf(dest, "/tmp/destdir");
	sprintf(src, "/tmp/src");
	rc = famfs_cp(&ll, src, dest, 0, 0, 0, 1);
	ASSERT_NE(rc, 0);

	system("touch /tmp/dest");
	sprintf(dest, "/tmp/dest");
	rc = famfs_cp(&ll, src, dest, 0, 0, 0, 1);
	ASSERT_NE(rc, 0);

	sprintf(dest, "/tmp/destdir");
	mock_failure = MOCK_FAIL_GENERIC;
	rc = famfs_cp(&ll, src, dest, 0, 0, 0, 1);
	ASSERT_NE(rc, 0);
	mock_failure = MOCK_FAIL_NONE;

	system("rm /tmp/dest");
	system("rmdir /tmp/destdir");
}

TEST(famfs, famfs_print_role_string) {
	/* Increase code coverage */
	famfs_print_role_string(FAMFS_MASTER);
	famfs_print_role_string(FAMFS_CLIENT);
	famfs_print_role_string(FAMFS_NOSUPER);
}
