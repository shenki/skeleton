/* Copyright 2016 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <byteswap.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <limits.h>
#include <arpa/inet.h>
#include <assert.h>
#include <mtd/mtd-user.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <openbmc_intf.h>
#include <openbmc.h>

static const gchar* dbus_object_path = "/org/openbmc/control";
static const gchar* dbus_name = "org.openbmc.control.Flasher";

static GDBusObjectManagerServer *manager = NULL;

#define __aligned(x)			__attribute__((aligned(x)))

static bool need_relock;

#define FILE_BUF_SIZE	0x10000
static uint8_t file_buf[FILE_BUF_SIZE] __aligned(0x1000);

static uint8_t FLASH_OK = 0;
static uint8_t FLASH_ERROR = 0x01;
static uint8_t FLASH_SETUP_ERROR = 0x02;

static int
erase_chip(int fd)
{
	int rc = 0;
	mtd_info_t mtd_info;
	struct erase_info_user erase;

	rc = ioctl(fd, MEMGETINFO, &mtd_info);
	if (rc < 0) {
		perror("could not get mtd size");
		return errno;
	}


	printf("Erasing... (may take a while !) ");
	fflush(stdout);

	erase.start = 0;
	erase.length = mtd_info.size;
	rc = ioctl(fd, MEMERASE, &erase);
	if (rc < 0) {
		perror("Error erasing chip");
		return errno;
	}

	printf("done !\n");

	return rc;
}

void
flash_message(GDBusConnection* connection,char* obj_path,char* method, char* error_msg)
{
	GDBusProxy *proxy;
	GError *error;
	GVariant *parm = NULL;
	error = NULL;
	proxy = g_dbus_proxy_new_sync(connection,
			G_DBUS_PROXY_FLAGS_NONE,
			NULL, /* GDBusInterfaceInfo* */
			"org.openbmc.control.Flash", /* name */
			obj_path, /* object path */
			"org.openbmc.Flash", /* interface name */
			NULL, /* GCancellable */
			&error);
	g_assert_no_error(error);

	error = NULL;
	if(strcmp(method,"error")==0) {
		parm = g_variant_new("(s)",error_msg);
	}
	g_dbus_proxy_call_sync(proxy,
			method,
			parm,
			G_DBUS_CALL_FLAGS_NONE,
			-1,
			NULL,
			&error);

	g_assert_no_error(error);
}

static int
program_file(FlashControl* flash_control, int in_fd, int out_fd, size_t size)
{
	int infd, outfd, rc = 0;
	ssize_t len;
	uint32_t actual_size = 0;
	unsigned int save_size = size;
	uint8_t last_progress = 0;

	while(size) {
		len = read(infd, file_buf, FILE_BUF_SIZE);
		if(len < 0) {
			perror("mtd write failed");
			return errno;
		}
		if(len == 0)
			break;
		if(len > size)
			len = size;
		size -= len;
		actual_size += len;
		rc = write(outfd, file_buf, len);
		if (rc) {
			fprintf(stderr,
				"Flash write error %d for chunk at 0x%08lx\n",
				rc, lseek(outfd, 0, SEEK_CUR));
			return errno;
		}
		uint8_t progress = 100 * actual_size/save_size;
		if (progress != last_progress) {
			/* TODO: second arg should be the file */
			flash_control_emit_progress(flash_control, NULL ,progress);
			last_progress = progress;
		}
	}

	return rc;
}

uint8_t
flash(FlashControl* flash_control,const char *mtd_path, const char *write_file, char* obj_path)
{
	bool erase = true, program = true;
	char *flash_path;
	int file_fd, flash_fd;
	int rc;

	flash_fd = open(mtd_path, O_RDONLY);
	if (flash_fd == -1) {
		perror(mtd_path);
		return errno;
	}

	file_fd = open(write_file, O_WRONLY);
	if (file_fd == -1) {
		perror(write_file);
		return errno;
	}

	if(strcmp(write_file,"")!=0)
	{
		// If file specified but not size, get size from file
		struct stat stbuf;
		if(stat(write_file, &stbuf)) {
			perror("Failed to get file size");
			return FLASH_ERROR;
		}
		uint32_t write_size = stbuf.st_size;

		rc = erase_chip(flash_fd);
		if (rc) {
			return FLASH_ERROR;
		}

		rc = program_file(flash_control, file_fd, flash_fd, write_size);
		if (rc) {
			return FLASH_ERROR;
		}

		printf("Flash done\n");
	}

	return FLASH_OK;
}

static void
on_bus_acquired(GDBusConnection *connection,
		const gchar *name,
		gpointer user_data)
{
	const char *mtd_path;
	cmdline *cmd = user_data;
	ObjectSkeleton *object;
	gchar *s;

	if (cmd->argc < 4) {
		g_print("flasher [flash name] [filename] [source object]\n");
		g_main_loop_quit(cmd->loop);
		return;
	}
	printf("Starting flasher: %s,%s,%s,\n",cmd->argv[1],cmd->argv[2],cmd->argv[3]);

	manager = g_dbus_object_manager_server_new(dbus_object_path);

	s = g_strdup_printf("%s/%s",dbus_object_path,cmd->argv[1]);
	object = object_skeleton_new(s);
	g_free(s);

	FlashControl* flash_control = flash_control_skeleton_new();
	object_skeleton_set_flash_control(object, flash_control);
	g_object_unref(flash_control);

	/* Export the object (@manager takes its own reference to @object) */
	g_dbus_object_manager_server_export(manager, G_DBUS_OBJECT_SKELETON(object));
	g_object_unref(object);

	/* Export all objects */
	g_dbus_object_manager_server_set_connection(manager, connection);

	if (strcmp(cmd->argv[1], "bmc") == 0) {
		/* TODO: fix this */
		mtd_path = "/dev/mtd0";
	}
	if (strcmp(cmd->argv[1], "bmc_ramdisk") == 0) {
		mtd_path = "/dev/mtd3";
	}
	if (strcmp(cmd->argv[1], "bmc_kernel") == 0) {
		mtd_path = "/dev/mtd2";
	}
	if (strcmp(cmd->argv[1], "pnor") == 0) {
		mtd_path = "/dev/mtd7";
	}

	int rc = flash(flash_control, mtd_path,cmd->argv[2],cmd->argv[3]);
	if(rc) {
		flash_message(connection,cmd->argv[3],"error","Flash Error");
	} else {
		flash_message(connection,cmd->argv[3],"done","");
	}

	//Object exits when done flashing
	g_main_loop_quit(cmd->loop);
}

int
main(int argc, char *argv[])
{
	GMainLoop *loop;
	cmdline cmd;
	cmd.argc = argc;
	cmd.argv = argv;

	guint id;
	loop = g_main_loop_new(NULL, FALSE);
	cmd.loop = loop;

	id = g_bus_own_name(DBUS_TYPE,
			dbus_name,
			G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
			G_BUS_NAME_OWNER_FLAGS_REPLACE,
			on_bus_acquired,
			NULL,
			NULL,
			&cmd,
			NULL);

	g_main_loop_run(loop);

	g_bus_unown_name(id);
	g_main_loop_unref(loop);

	return 0;
}
