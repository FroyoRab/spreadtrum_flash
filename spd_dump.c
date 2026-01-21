/*
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
*/
#include "common.h"
#include "GITVER.h"

void print_help(void) {
	DBG_LOG(
		"Usage\n"
		"\tspd_dump [OPTIONS]\n"
		"\nThis build runs a fixed batch sequence on every detected device:\n"
		"\tfdl fdl1-sign.bin 0x5500\n"
		"\tfdl fdl2-sign.bin 0x9efffe00\n"
		"\texec\n"
		"\tw splloader splloader.bin\n"
		"\tw vbmeta vbmeta.img\n"
		"\tw boot boot.img\n"
		"\treset\n"
	);
	DBG_LOG(
		"\nOptions\n"
		"\t--wait <seconds> [count]\n"
		"\t\tSpecifies the time to wait for devices to connect. When count is set,\n"
		"\t\tcontinue only after that many devices are detected.\n"
		"\t--verbose <level>\n"
		"\t\tSets the verbosity level of the output (supports 0, 1, or 2).\n"
		"\t-?|-h|--help\n"
		"\t\tShow help and usage information\n"
	);
}

#define REOPEN_FREQ 2
extern char fn_partlist[40];
extern char savepath[ARGV_LEN];
extern DA_INFO_T Da_Info;
extern partition_t gPartInfo;
int bListenLibusb = -1;
int gpt_failed = 1;
int m_bOpened = 0;
int fdl1_loaded = 0;
int fdl2_executed = 0;
int selected_ab = -1;
uint64_t fblk_size = 0;
static void reset_runtime_state(void) {
	fdl1_loaded = 0;
	fdl2_executed = 0;
	selected_ab = -1;
	gpt_failed = 1;
	fblk_size = 0;
	memset(&Da_Info, 0, sizeof(Da_Info));
	memset(&gPartInfo, 0, sizeof(gPartInfo));
	memset(savepath, 0, sizeof(savepath));
}

static int run_batch_sequence(int argc, char **argv, int wait, int verbose) {
	spdio_t *io = NULL; int ret, i, in_quote;
	int argcount = 0, stage = -1, nand_id = DEFAULT_NAND_ID;
	int nand_info[3];
	int keep_charge = 1, end_data = 1, blk_size = 0, skip_confirm = 1, highspeed = 0, exec_addr_v2 = 0;
	unsigned exec_addr = 0, baudrate = 0;
	char **str2;
	char *execfile;
	int bootmode = -1, at = 0, async = 1;
	const useconds_t command_delay_us = 200000;
#if !USE_LIBUSB
	extern DWORD curPort;
	DWORD *ports;
#else
	extern libusb_device *curPort;
	libusb_device **ports;
#endif
	execfile = malloc(ARGV_LEN);
	if (!execfile) ERR_EXIT("malloc failed\n");

	io = spdio_init(0);
	io->verbose = verbose;
#if USE_LIBUSB
#ifdef __ANDROID__
	int xfd = -1; // This store termux gived fd
	//libusb_device_handle *handle; // Use spdio_t.dev_handle
	//libusb_device* device; //use curPort
	struct libusb_device_descriptor desc;
	libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
#endif
	ret = libusb_init(NULL);
	if (ret < 0)
		ERR_EXIT("libusb_init failed: %s\n", libusb_error_name(ret));
#else
	io->handle = createClass();
	call_Initialize(io->handle);
#endif
	DBG_LOG("branch:%s, sha1:%s\n", GIT_VER, GIT_SHA1);
	sprintf(fn_partlist, "partition_%lld.xml", (long long)time(NULL));
#if defined(_MYDEBUG) && defined(USE_LIBUSB)
	io->verbose = 2;
#endif
	if (stage == 99) { bootmode = -1; at = 0; }
#ifdef __ANDROID__
	bListenLibusb = 0;
	DBG_LOG("Try to convert termux transfered usb port fd.\n");
	// handle
	if (xfd < 0)
		ERR_EXIT("Example: termux-usb -e \"./spd_dump --usb-fd\" /dev/bus/usb/xxx/xxx\n"
			"run on android need provide --usb-fd\n");

	if (libusb_wrap_sys_device(NULL, (intptr_t)xfd, &io->dev_handle))
		ERR_EXIT("libusb_wrap_sys_device exit unconditionally!\n");

	curPort = libusb_get_device(io->dev_handle);
	if (libusb_get_device_descriptor(curPort, &desc))
		ERR_EXIT("libusb_get_device exit unconditionally!");

	DBG_LOG("Vendor ID: %04x\nProduct ID: %04x\n", desc.idVendor, desc.idProduct);
	if (desc.idVendor != 0x1782 || desc.idProduct != 0x4d00) {
		ERR_EXIT("It seems spec device not a spd device!\n");
	}
	call_Initialize_libusb(io);
#else
#if !USE_LIBUSB
	bListenLibusb = 0;
	if (at || bootmode >= 0) {
		io->hThread = CreateThread(NULL, 0, ThrdFunc, NULL, 0, &io->iThread);
		if (io->hThread == NULL) return -1;
		ChangeMode(io, wait / REOPEN_FREQ * 1000, bootmode, at);
		wait = 30 * REOPEN_FREQ;
		stage = -1;
	}
#else
	if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) { DBG_LOG("hotplug unsupported on this platform\n"); bListenLibusb = 0; bootmode = -1; at = 0; }
	if (at || bootmode >= 0) {
		startUsbEventHandle();
		ChangeMode(io, wait / REOPEN_FREQ * 1000, bootmode, at);
		wait = 30 * REOPEN_FREQ;
		stage = -1;
	}
	if (bListenLibusb < 0) startUsbEventHandle();
#endif
#if _WIN32
	if (!bListenLibusb) {
		if (io->hThread == NULL) io->hThread = CreateThread(NULL, 0, ThrdFunc, NULL, 0, &io->iThread);
		if (io->hThread == NULL) return -1;
	}
#if !USE_LIBUSB
	if (!m_bOpened && async) {
		if (FALSE == CreateRecvThread(io)) {
			io->m_dwRecvThreadID = 0;
			DBG_LOG("Create Receive Thread Fail.\n");
		}
	}
#endif
#endif
	if (!m_bOpened) {
		DBG_LOG("Waiting for dl_diag connection (%ds)\n", wait / REOPEN_FREQ);
		for (i = 0; ; i++) {
#if USE_LIBUSB
			if (bListenLibusb) {
				if (curPort) {
					if (libusb_open(curPort, &io->dev_handle) >= 0) call_Initialize_libusb(io);
					else ERR_EXIT("Connection failed\n");
					break;
				}
			}
			if (!(i % 4)) {
				if ((ports = FindPort(0x4d00))) {
					for (libusb_device **port = ports; *port != NULL; port++) {
						if (libusb_open(*port, &io->dev_handle) >= 0) {
							call_Initialize_libusb(io);
							curPort = *port;
							break;
						}
					}
					libusb_free_device_list(ports, 1);
					ports = NULL;
					if (m_bOpened) break;
				}
			}
			if (i >= wait)
				ERR_EXIT("libusb_open_device failed\n");
#else
			if (io->verbose) DBG_LOG("CurTime: %.1f, CurPort: %d\n", (float)i / REOPEN_FREQ, curPort);
			if (curPort) {
				if (!call_ConnectChannel(io->handle, curPort, WM_RCV_CHANNEL_DATA, io->m_dwRecvThreadID)) ERR_EXIT("Connection failed\n");
				break;
			}
			if (!(i % 4)) {
				if ((ports = FindPort("SPRD U2S Diag"))) {
					for (DWORD *port = ports; *port != 0; port++) {
						if (call_ConnectChannel(io->handle, *port, WM_RCV_CHANNEL_DATA, io->m_dwRecvThreadID)) {
							curPort = *port;
							break;
						}
					}
					free(ports);
					ports = NULL;
					if (m_bOpened) break;
				}
			}
			if (i >= wait)
				ERR_EXIT("find port failed\n");
#endif
			usleep(1000000 / REOPEN_FREQ);
		}
	}
#endif
	io->flags |= FLAGS_TRANSCODE;
	if (stage != -1) {
		io->flags &= ~FLAGS_CRC16;
		encode_msg_nocpy(io, BSL_CMD_CONNECT, 0);
	}
	else encode_msg(io, BSL_CMD_CHECK_BAUD, NULL, 1);
	for (i = 0; ; i++) {
		if (io->recv_buf[2] == BSL_REP_VER) {
			ret = BSL_REP_VER;
			memcpy(io->raw_buf + 4, io->recv_buf + 5, 5);
			io->raw_buf[2] = 0;
			io->raw_buf[3] = 5;
			io->recv_buf[2] = 0;
		}
		else if (io->recv_buf[2] == BSL_REP_VERIFY_ERROR ||
			io->recv_buf[2] == BSL_REP_UNSUPPORTED_COMMAND) {
			if (!fdl1_loaded) {
				ret = io->recv_buf[2];
				io->recv_buf[2] = 0;
			}
			else ERR_EXIT("wrong command or wrong mode detected, reboot your phone by pressing POWER and VOL_UP for 7-10 seconds.\n");
		}
		else {
			send_msg(io);
			recv_msg(io);
			ret = recv_type(io);
		}
		if (ret == BSL_REP_ACK || ret == BSL_REP_VER || ret == BSL_REP_VERIFY_ERROR) {
			if (ret == BSL_REP_VER) {
				if (fdl1_loaded == 1) {
					DBG_LOG("CHECK_BAUD FDL1\n");
					if (!memcmp(io->raw_buf + 4, "SPRD4", 5)) fdl2_executed = -1;
				}
				else {
					DBG_LOG("CHECK_BAUD bootrom\n");
					if (!memcmp(io->raw_buf + 4, "SPRD4", 5)) { fdl1_loaded = -1; fdl2_executed = -1; }
				}
				DBG_LOG("BSL_REP_VER: ");
				print_string(stderr, io->raw_buf + 4, READ16_BE(io->raw_buf + 2));

				encode_msg_nocpy(io, BSL_CMD_CONNECT, 0);
				if (send_and_check(io)) exit(1);
			}
			else if (ret == BSL_REP_VERIFY_ERROR) {
				encode_msg_nocpy(io, BSL_CMD_CONNECT, 0);
				if (fdl1_loaded != 1) {
					if (send_and_check(io)) exit(1);
				}
				else { i = -1; continue; }
			}

			if (fdl1_loaded == 1) {
				DBG_LOG("CMD_CONNECT FDL1\n");
				if (keep_charge) {
					encode_msg_nocpy(io, BSL_CMD_KEEP_CHARGE, 0);
					if (!send_and_check(io)) DBG_LOG("KEEP_CHARGE FDL1\n");
				}
			}
			else DBG_LOG("CMD_CONNECT bootrom\n");
			break;
		}
		else if (ret == BSL_REP_UNSUPPORTED_COMMAND) {
			encode_msg_nocpy(io, BSL_CMD_DISABLE_TRANSCODE, 0);
			if (!send_and_check(io)) {
				io->flags &= ~FLAGS_TRANSCODE;
				DBG_LOG("DISABLE_TRANSCODE\n");
			}
			fdl2_executed = 1;
			break;
		}
		else if (i == 4) {
			if (stage != -1) ERR_EXIT("wrong command or wrong mode detected, reboot your phone by pressing POWER and VOL_UP for 7-10 seconds.\n");
			else { encode_msg_nocpy(io, BSL_CMD_CONNECT, 0); stage++; i = -1; }
		}
	}

	char **save_argv = NULL;
	if (fdl1_loaded == -1) argc += 2;
	if (fdl2_executed == -1) argc += 1;
	while (1) {
		if (argc <= 1) break;
		str2 = (char **)malloc(argc * sizeof(char *));
		if (fdl1_loaded == -1) {
			save_argv = argv;
			str2[1] = "loadfdl";
			str2[2] = "0x0";
		}
		else if (fdl2_executed == -1) {
			if (!save_argv) save_argv = argv;
			str2[1] = "exec";
		}
		else {
			if (save_argv) { argv = save_argv; save_argv = NULL; }
			for (i = 1; i < argc; i++) str2[i] = argv[i];
		}
		argcount = argc;
		in_quote = -1;
		if (argcount == 1) {
			str2[1] = malloc(1);
			if (str2[1]) str2[1][0] = '\0';
			else ERR_EXIT("malloc failed\n");
			argcount++;
		}

		if (!strcmp(str2[1], "sendloop")) {
			uint32_t addr = 0;
			if (argcount <= 2) { DBG_LOG("sendloop addr\n"); argc = 1; continue; }

			uint8_t data[4] = { 0 };
			addr = strtoul(str2[2], NULL, 0);
			while (1) {
				send_buf(io, addr, 0, 528, data, 4);
				DBG_LOG("SEND 4 bytes to 0x%x\n", addr);
				addr -= 8;
			}
			argc -= 2; argv += 2;
		}
		else if (!strcmp(str2[1], "write_word")) {
			uint32_t addr, data;
			if (argc <= 3) { DBG_LOG("write_word addr VALUE(max is 0xFFFFFFFF)\n"); argc = 1; continue; }

			addr = strtoul(str2[2], NULL, 0);
			data = strtoul(str2[3], NULL, 0);
			send_buf(io, addr, end_data, 528, (uint8_t *)&data, 4);
			argc -= 3; argv += 3;

		}
		else if (!strcmp(str2[1], "send") || !strcmp(str2[1], "write_flash")) {
			const char *fn; uint32_t addr = 0; FILE *fi;
			if (argcount <= 3) { DBG_LOG("send|write_flash FILE addr\n"); argc = 1; continue; }

			fn = str2[2];
			fi = fopen(fn, "r");
			if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= 3; argv += 3; continue; }
			else fclose(fi);
			addr = strtoul(str2[3], NULL, 0);
			if (!strcmp(str2[1], "send")) send_file(io, fn, addr, 0, 528, 0, 0);
			else send_file(io, fn, addr, end_data, 528, 0, 0);
			argc -= 3; argv += 3;
		}
		else if (!strncmp(str2[1], "fdl", 3) || !strncmp(str2[1], "loadfdl", 7)) {
			const char *fn; uint32_t addr = 0; FILE *fi;
			int addr_in_name = !strncmp(str2[1], "loadfdl", 7);
			int argchange;

			fn = str2[2];
			if (addr_in_name) {
				argchange = 2;
				if (argcount <= argchange) { DBG_LOG("loadfdl FILE\n"); argc = 1; continue; }
				char *pos = NULL, *last_pos = NULL;

				pos = strstr(fn, "0X");
				while (pos) {
					last_pos = pos;
					pos = strstr(pos + 2, "0X");
				}
				if (last_pos == NULL) {
					pos = strstr(fn, "0x");
					while (pos) {
						last_pos = pos;
						pos = strstr(pos + 2, "0x");
					}
				}
				if (last_pos) addr = strtoul(last_pos, NULL, 16);
				else { DBG_LOG("\"0x\" not found in name.\n"); argc -= argchange; argv += argchange; continue; }
			}
			else {
				argchange = 3;
				if (argcount <= argchange) { DBG_LOG("fdl FILE addr\n"); argc = 1; continue; }
				addr = strtoul(str2[3], NULL, 0);
			}

			if (fdl2_executed > 0) {
				DBG_LOG("FDL2 ALREADY EXECUTED, SKIP\n");
				argc -= argchange; argv += argchange;
				continue;
			}
			else if (fdl1_loaded > 0) {
				if (fdl2_executed != -1) {
					fi = fopen(fn, "r");
					if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= argchange; argv += argchange; continue; }
					else fclose(fi);
					send_file(io, fn, addr, end_data, blk_size ? blk_size : 528, 0, 0);
				}
			}
			else {
				if (fdl1_loaded != -1) {
					fi = fopen(fn, "r");
					if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= argchange; argv += argchange; continue; }
					else fclose(fi);
					if (exec_addr_v2) {
						size_t execsize = send_file(io, fn, addr, 0, 528, 0, 0);
						int n, gapsize = exec_addr - addr - execsize;
						for (i = 0; i < gapsize; i += n) {
							n = gapsize - i;
							if (n > 528) n = 528;
							encode_msg_nocpy(io, BSL_CMD_MIDST_DATA, n);
							if (send_and_check(io)) exit(1);
						}
						fi = fopen(execfile, "rb");
						if (fi) {
							fseek(fi, 0, SEEK_END);
							n = ftell(fi);
							fseek(fi, 0, SEEK_SET);
							execsize = fread(io->temp_buf, 1, n, fi);
							fclose(fi);
						}
						encode_msg_nocpy(io, BSL_CMD_MIDST_DATA, execsize);
						if (send_and_check(io)) exit(1);
						free(execfile);
					}
					else {
						send_file(io, fn, addr, end_data, 528, 0, 0);
						if (exec_addr) {
							send_file(io, execfile, exec_addr, 0, 528, 0, 0);
							free(execfile);
						}
						else {
							encode_msg_nocpy(io, BSL_CMD_EXEC_DATA, 0);
							if (send_and_check(io)) exit(1);
						}
					}
				}
				else {
					encode_msg_nocpy(io, BSL_CMD_EXEC_DATA, 0);
					if (send_and_check(io)) exit(1);
				}
				DBG_LOG("EXEC FDL1\n");
				if (addr == 0x5500 || addr == 0x65000800) {
					highspeed = 1;
					if (!baudrate) baudrate = 921600;
				}

				/* FDL1 (chk = sum) */
				io->flags &= ~FLAGS_CRC16;

				encode_msg(io, BSL_CMD_CHECK_BAUD, NULL, 1);
				for (i = 0; ; i++) {
					send_msg(io);
					recv_msg(io);
					if (recv_type(io) == BSL_REP_VER) break;
					DBG_LOG("CHECK_BAUD FAIL\n");
					if (i == 4) ERR_EXIT("wrong command or wrong mode detected, reboot your phone by pressing POWER and VOL_UP for 7-10 seconds.\n");
					usleep(500000);
				}
				DBG_LOG("CHECK_BAUD FDL1\n");

				DBG_LOG("BSL_REP_VER: ");
				print_string(stderr, io->raw_buf + 4, READ16_BE(io->raw_buf + 2));
				if (!memcmp(io->raw_buf + 4, "SPRD4", 5)) fdl2_executed = -1;

#if FDL1_DUMP_MEM
				//read dump mem
				int pagecount = 0;
				char *pdump;
				char chdump;
				FILE *fdump;
				fdump = my_fopen("memdump.bin", "wb");
				encode_msg(io, BSL_CMD_CHECK_BAUD, NULL, 1);
				while (1) {
					send_msg(io);
					ret = recv_msg(io);
					if (!ret) ERR_EXIT("timeout reached\n");
					if (recv_type(io) == BSL_CMD_READ_END) break;
					pdump = (char *)(io->raw_buf + 4);
					for (i = 0; i < 512; i++) {
						chdump = *(pdump++);
						if (chdump == 0x7d) {
							if (*pdump == 0x5d || *pdump == 0x5e) chdump = *(pdump++) + 0x20;
						}
						fputc(chdump, fdump);
					}
					DBG_LOG("dump page count %d\n", ++pagecount);
				}
				fclose(fdump);
				DBG_LOG("dump mem end\n");
				//end
#endif

				encode_msg_nocpy(io, BSL_CMD_CONNECT, 0);
				if (send_and_check(io)) exit(1);
				DBG_LOG("CMD_CONNECT FDL1\n");
#if !USE_LIBUSB
				if (baudrate) {
					uint8_t *data = io->temp_buf;
					WRITE32_BE(data, baudrate);
					encode_msg_nocpy(io, BSL_CMD_CHANGE_BAUD, 4);
					if (!send_and_check(io)) {
						DBG_LOG("CHANGE_BAUD FDL1 to %d\n", baudrate);
						call_SetProperty(io->handle, 0, 100, (LPCVOID)&baudrate);
					}
				}
#endif
				if (keep_charge) {
					encode_msg_nocpy(io, BSL_CMD_KEEP_CHARGE, 0);
					if (!send_and_check(io)) DBG_LOG("KEEP_CHARGE FDL1\n");
				}
				fdl1_loaded = 1;
			}
			argc -= argchange; argv += argchange;

		}
		else if (!strcmp(str2[1], "exec")) {
			if (fdl2_executed > 0) {
				DBG_LOG("FDL2 ALREADY EXECUTED, SKIP\n");
				argc -= 1; argv += 1;
				continue;
			}
			else if (fdl1_loaded > 0) {
				memset(&Da_Info, 0, sizeof(Da_Info));
				encode_msg_nocpy(io, BSL_CMD_EXEC_DATA, 0);
				send_msg(io);
				// Feature phones respond immediately,
				// but it may take a second for a smartphone to respond.
				ret = recv_msg_timeout(io, 15000);
				if (!ret) ERR_EXIT("timeout reached\n");
				ret = recv_type(io);
				// Is it always bullshit?
				if (ret == BSL_REP_INCOMPATIBLE_PARTITION)
					get_Da_Info(io);
				else if (ret != BSL_REP_ACK)
					ERR_EXIT("unexpected response (0x%04x)\n", ret);
				DBG_LOG("EXEC FDL2\n");
				encode_msg_nocpy(io, BSL_CMD_READ_FLASH_INFO, 0);
				send_msg(io);
				ret = recv_msg(io);
				if (ret) {
					ret = recv_type(io);
					if (ret != BSL_REP_READ_FLASH_INFO) DBG_LOG("unexpected response (0x%04x)\n", ret);
					else Da_Info.dwStorageType = 0x101;
					// need more samples to cover BSL_REP_READ_MCP_TYPE packet to nand_id/nand_info
					// for nand_id 0x15, packet is 00 9b 00 0c 00 00 00 00 00 02 00 00 00 00 08 00
				}
				if (Da_Info.bDisableHDLC) {
					encode_msg_nocpy(io, BSL_CMD_DISABLE_TRANSCODE, 0);
					if (!send_and_check(io)) {
						io->flags &= ~FLAGS_TRANSCODE;
						DBG_LOG("DISABLE_TRANSCODE\n");
					}
				}
				if (Da_Info.bSupportRawData) {
					blk_size = 0xf800;
					io->ptable = partition_list(io, fn_partlist, &io->part_count);
					if (fdl2_executed) {
						Da_Info.bSupportRawData = 0;
						DBG_LOG("DISABLE_WRITE_RAW_DATA in SPRD4\n");
					}
					else {
						encode_msg_nocpy(io, BSL_CMD_ENABLE_RAW_DATA, 0);
						if (!send_and_check(io)) DBG_LOG("ENABLE_WRITE_RAW_DATA\n");
					}
				}
				else if (highspeed || Da_Info.dwStorageType == 0x103) {
					blk_size = 0xf800;
					io->ptable = partition_list(io, fn_partlist, &io->part_count);
				}
				else if (Da_Info.dwStorageType == 0x102) {
					io->ptable = partition_list(io, fn_partlist, &io->part_count);
				}
				else if (Da_Info.dwStorageType == 0x101) DBG_LOG("Storage is nand\n");
				if (gpt_failed != 1) {
					if (selected_ab == 2) DBG_LOG("Device is using slot b\n");
					else if (selected_ab == 1) DBG_LOG("Device is using slot a\n");
					else {
						DBG_LOG("Device is not using VAB\n");
						if (Da_Info.bSupportRawData) {
							DBG_LOG("RAW_DATA level is %u, but DISABLED for stability, you can set it manually\n", (unsigned)Da_Info.bSupportRawData);
							Da_Info.bSupportRawData = 0;
						}
					}
				}
				if (nand_id == DEFAULT_NAND_ID) {
					nand_info[0] = (uint8_t)pow(2, nand_id & 3); //page size
					nand_info[1] = 32 / (uint8_t)pow(2, (nand_id >> 2) & 3); //spare area size
					nand_info[2] = 64 * (uint8_t)pow(2, (nand_id >> 4) & 3); //block size
				}
				fdl2_executed = 1;
			}
			argc -= 1; argv += 1;
#if !USE_LIBUSB
		}
		else if (!strcmp(str2[1], "baudrate")) {
			if (argcount > 2) {
				baudrate = strtoul(str2[2], NULL, 0);
				if (fdl2_executed) call_SetProperty(io->handle, 0, 100, (LPCVOID)&baudrate);
			}
			DBG_LOG("baudrate is %u\n", baudrate);
			argc -= 2; argv += 2;
#endif
		}
		else if (!strcmp(str2[1], "path")) {
			if (argcount > 2) strcpy(savepath, str2[2]);
			DBG_LOG("save dir is %s\n", savepath);
			argc -= 2; argv += 2;

		}
		else if (!strncmp(str2[1], "exec_addr", 9)) {
			FILE *fi;
			if (0 == fdl1_loaded && argcount > 2) {
				exec_addr = strtoul(str2[2], NULL, 0);
				sprintf(execfile, "custom_exec_no_verify_%x.bin", exec_addr);
				fi = fopen(execfile, "r");
				if (fi == NULL) { DBG_LOG("%s does not exist\n", execfile); exec_addr = 0; }
				else fclose(fi);
			}
			DBG_LOG("current exec_addr is 0x%x\n", exec_addr);
			if (!strcmp(str2[1], "exec_addr2")) exec_addr_v2 = 1;
			argc -= 2; argv += 2;
		}
		else if (!strncmp(str2[1], "loadexec", 8)) {
			const char *fn; char *ch; FILE *fi;
			if (argcount <= 2) { DBG_LOG("loadexec FILE\n"); argc = 1; continue; }
			if (0 == fdl1_loaded) {
				strcpy(execfile, str2[2]);

				if ((ch = strrchr(execfile, '/'))) fn = ch + 1;
				else if ((ch = strrchr(execfile, '\\'))) fn = ch + 1;
				else fn = execfile;
				char straddr[9] = { 0 };
				ret = sscanf(fn, "custom_exec_no_verify_%[0-9a-fA-F]", straddr);
				exec_addr = strtoul(straddr, NULL, 16);
				fi = fopen(execfile, "r");
				if (fi == NULL) { DBG_LOG("%s does not exist\n", execfile); exec_addr = 0; }
				else fclose(fi);
			}
			DBG_LOG("current exec_addr is 0x%x\n", exec_addr);
			if (!strcmp(str2[1], "loadexec2")) exec_addr_v2 = 1;
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "nand_id")) {
			if (argcount > 2) {
				nand_id = strtol(str2[2], NULL, 0);
				nand_info[0] = (uint8_t)pow(2, nand_id & 3); //page size
				nand_info[1] = 32 / (uint8_t)pow(2, (nand_id >> 2) & 3); //spare area size
				nand_info[2] = 64 * (uint8_t)pow(2, (nand_id >> 4) & 3); //block size
			}
			DBG_LOG("current nand_id is 0x%x\n", nand_id);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "read_flash")) {
			const char *fn; uint64_t addr, offset, size;
			if (argcount <= 5) { DBG_LOG("read_flash addr offset size FILE\n"); argc = 1; continue; }

			addr = str_to_size(str2[2]);
			offset = str_to_size(str2[3]);
			size = str_to_size(str2[4]);
			fn = str2[5];
			if ((addr | size | offset | (addr + offset + size)) >> 32) { DBG_LOG("32-bit limit reached\n"); argc -= 5; argv += 5; continue; }
			dump_flash(io, addr, offset, size, fn, blk_size ? blk_size : 1024);
			argc -= 5; argv += 5;

		}
		else if (!strcmp(str2[1], "erase_flash")) {
			uint64_t addr, size;
			if (argc <= 3) { DBG_LOG("erase_flash addr size\n"); argc = 1; continue; }
			if (!skip_confirm)
				if (!check_confirm("erase flash")) {
					argc -= 3; argv += 3;
					continue;
				}
			addr = str_to_size(str2[2]);
			size = str_to_size(str2[3]);
			if ((addr | size | (addr + size)) >> 32)
				ERR_EXIT("32-bit limit reached\n");
			uint32_t *data = (uint32_t *)io->temp_buf;
			WRITE32_BE(data, addr);
			WRITE32_BE(data + 1, size);
			encode_msg_nocpy(io, BSL_CMD_ERASE_FLASH, 4 * 2);
			if (!send_and_check(io)) DBG_LOG("Erase Flash Done: 0x%08x\n", (int)addr);
			argc -= 3; argv += 3;

		}
		else if (!strcmp(str2[1], "read_mem")) {
			const char *fn; uint64_t addr, size;
			if (argcount <= 4) { DBG_LOG("read_mem addr size FILE\n"); argc = 1; continue; }

			addr = str_to_size(str2[2]);
			size = str_to_size(str2[3]);
			fn = str2[4];
			if ((addr | size | (addr + size)) >> 32) { DBG_LOG("32-bit limit reached\n"); argc -= 4; argv += 4; continue; }
			dump_mem(io, addr, size, fn, blk_size ? blk_size : 1024);
			argc -= 4; argv += 4;

		}
		else if (!strcmp(str2[1], "part_size") || !strcmp(str2[1], "size_part")) {
			const char *name;
			if (argcount <= 2) { DBG_LOG("size_part part_name\n"); argc = 1; continue; }

			name = str2[2];
			if (selected_ab < 0) select_ab(io);
			DBG_LOG("%lld\n", (long long)check_partition(io, name, 1));
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "p") || !strcmp(str2[1], "print")) {
			if (io->part_count) {
				DBG_LOG("  0 %36s     256KB\n", "splloader");
				for (i = 0; i < io->part_count; i++) {
					DBG_LOG("%3d %36s %7lldMB\n", i + 1, (*(io->ptable + i)).name, ((*(io->ptable + i)).size >> 20));
				}
			}
			argc -= 1; argv += 1;

		}
		else if (!strcmp(str2[1], "check_part")) {
			const char *name;
			if (argcount <= 2) { DBG_LOG("check_part part_name\n"); argc = 1; continue; }

			name = str2[2];
			if (selected_ab < 0) select_ab(io);
			DBG_LOG("%lld\n", (long long)check_partition(io, name, 0));
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "read_part")) {
			const char *name, *fn; uint64_t offset, size;
			uint64_t realsize = 0;
			if (argcount <= 5) { DBG_LOG("read_part part_name offset size FILE\n(read ubi on nand) read_part system 0 ubi40m system.bin\n"); argc = 1; continue; }

			offset = str_to_size_ubi(str2[3], nand_info);
			size = str_to_size_ubi(str2[4], nand_info);
			fn = str2[5];

			name = str2[2];
			get_partition_info(io, name, 0);
			if (!gPartInfo.size) { DBG_LOG("part not exist\n"); argc -= 5; argv += 5; continue; }

			if (0xffffffff == size) size = check_partition(io, gPartInfo.name, 1);
			if (offset + size < offset) { DBG_LOG("64-bit limit reached\n"); argc -= 5; argv += 5; continue; }
			dump_partition(io, gPartInfo.name, offset, size, fn, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			argc -= 5; argv += 5;

		}
		else if (!strcmp(str2[1], "r")) {
			const char *name = str2[2];
			int loop_count = 0, in_loop = 0;
			const char *list[] = { "vbmeta", "splloader", "uboot", "sml", "trustos", "teecfg", "boot", "recovery" };
			if (argcount <= 2) { DBG_LOG("r all/all_lite/part_name/part_id\n"); argc = 1; continue; }
			if (!strcmp(name, "preset_modem")) {
				if (gpt_failed == 1) io->ptable = partition_list(io, fn_partlist, &io->part_count);
				if (!io->part_count) { DBG_LOG("Partition table not available\n"); argc -= 2; argv += 2; continue; }
				if (selected_ab > 0) { DBG_LOG("saving slot info\n"); dump_partition(io, "misc", 0, 1048576, "misc.bin", blk_size); }
				for (i = 0; i < io->part_count; i++)
					if (0 == strncmp("l_", (*(io->ptable + i)).name, 2) || 0 == strncmp("nr_", (*(io->ptable + i)).name, 3)) {
						char dfile[40];
						snprintf(dfile, sizeof(dfile), "%s.bin", (*(io->ptable + i)).name);
						dump_partition(io, (*(io->ptable + i)).name, 0, (*(io->ptable + i)).size, dfile, blk_size ? blk_size : DEFAULT_BLK_SIZE);
					}
				argc -= 2; argv += 2;
				continue;
			}
			else if (!strcmp(name, "all")) {
				if (gpt_failed == 1) io->ptable = partition_list(io, fn_partlist, &io->part_count);
				if (!io->part_count) { DBG_LOG("Partition table not available\n"); argc -= 2; argv += 2; continue; }
				dump_partition(io, "splloader", 0, 256 * 1024, "splloader.bin", blk_size ? blk_size : DEFAULT_BLK_SIZE);
				for (i = 0; i < io->part_count; i++) {
					char dfile[40];
					if (!strncmp((*(io->ptable + i)).name, "blackbox", 8)) continue;
					else if (!strncmp((*(io->ptable + i)).name, "cache", 5)) continue;
					else if (!strncmp((*(io->ptable + i)).name, "userdata", 8)) continue;
					snprintf(dfile, sizeof(dfile), "%s.bin", (*(io->ptable + i)).name);
					dump_partition(io, (*(io->ptable + i)).name, 0, (*(io->ptable + i)).size, dfile, blk_size ? blk_size : DEFAULT_BLK_SIZE);
				}
				argc -= 2; argv += 2;
				continue;
			}
			else if (!strcmp(name, "all_lite")) {
				if (gpt_failed == 1) io->ptable = partition_list(io, fn_partlist, &io->part_count);
				if (!io->part_count) { DBG_LOG("Partition table not available\n"); argc -= 2; argv += 2; continue; }
				dump_partition(io, "splloader", 0, 256 * 1024, "splloader.bin", blk_size ? blk_size : DEFAULT_BLK_SIZE);
				for (i = 0; i < io->part_count; i++) {
					char dfile[40];
					size_t namelen = strlen((*(io->ptable + i)).name);
					if (!strncmp((*(io->ptable + i)).name, "blackbox", 8)) continue;
					else if (!strncmp((*(io->ptable + i)).name, "cache", 5)) continue;
					else if (!strncmp((*(io->ptable + i)).name, "userdata", 8)) continue;
					if (selected_ab == 1 && namelen > 2 && 0 == strcmp((*(io->ptable + i)).name + namelen - 2, "_b")) continue;
					else if (selected_ab == 2 && namelen > 2 && 0 == strcmp((*(io->ptable + i)).name + namelen - 2, "_a")) continue;
					snprintf(dfile, sizeof(dfile), "%s.bin", (*(io->ptable + i)).name);
					dump_partition(io, (*(io->ptable + i)).name, 0, (*(io->ptable + i)).size, dfile, blk_size ? blk_size : DEFAULT_BLK_SIZE);
				}
				argc -= 2; argv += 2;
				continue;
			}
			else {
				if (!strcmp(name, "preset_resign")) {
					loop_count = 7; name = list[loop_count]; in_loop = 1;
				}
rloop:
				get_partition_info(io, name, 1);
				if (!gPartInfo.size) {
					if (loop_count) { name = list[--loop_count]; goto rloop; }
					DBG_LOG("part not exist\n");
					argc -= 2; argv += 2;
					continue;
				}
			}
			char dfile[40];
			if (isdigit(str2[2][0])) snprintf(dfile, sizeof(dfile), "%s.bin", gPartInfo.name);
			else if (in_loop) snprintf(dfile, sizeof(dfile), "%s.bin", list[loop_count]);
			else snprintf(dfile, sizeof(dfile), "%s.bin", name);
			dump_partition(io, gPartInfo.name, 0, gPartInfo.size, dfile, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			if (loop_count--) { name = list[loop_count]; goto rloop; }
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "read_parts")) {
			const char *fn; FILE *fi;
			if (argcount <= 2) { DBG_LOG("read_parts partition_list_file\n"); argc = 1; continue; }
			fn = str2[2];
			fi = fopen(fn, "r");
			if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= 2; argv += 2; continue; }
			else fclose(fi);
			dump_partitions(io, fn, nand_info, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "partition_list")) {
			if (argcount <= 2) { DBG_LOG("partition_list FILE\n"); argc = 1; continue; }
			if (gpt_failed == 1) io->ptable = partition_list(io, str2[2], &io->part_count);
			if (!io->part_count) { DBG_LOG("Partition table not available\n"); argc -= 2; argv += 2; continue; }
			else {
				DBG_LOG("  0 %36s     256KB\n", "splloader");
				FILE *fo = my_fopen(str2[2], "wb");
				if (!fo) ERR_EXIT("fopen failed\n");
				fprintf(fo, "<Partitions>\n");
				for (i = 0; i < io->part_count; i++) {
					DBG_LOG("%3d %36s %7lldMB\n", i + 1, (*(io->ptable + i)).name, ((*(io->ptable + i)).size >> 20));
					fprintf(fo, "    <Partition id=\"%s\" size=\"", (*(io->ptable + i)).name);
					if (i + 1 == io->part_count) fprintf(fo, "0x%x\"/>\n", ~0);
					else fprintf(fo, "%lld\"/>\n", ((*(io->ptable + i)).size >> 20));
				}
				fprintf(fo, "</Partitions>");
				fclose(fo);
			}
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "repartition")) {
			const char *fn; FILE *fi;
			if (argcount <= 2) { DBG_LOG("repartition FILE\n"); argc = 1; continue; }
			fn = str2[2];
			fi = fopen(fn, "r");
			if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= 2; argv += 2; continue; }
			else fclose(fi);
			if (skip_confirm) repartition(io, str2[2]);
			else if (check_confirm("repartition")) repartition(io, str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "erase_part") || !strcmp(str2[1], "e")) {
			const char *name = str2[2];
			if (argcount <= 2) { DBG_LOG("erase_part part_name/part_id\n"); argc = 1; continue; }
			if (!strcmp(name, "all")) {
				if (!check_confirm("erase all")) {
					argc -= 2; argv += 2;
					continue;
				}
				strcpy(gPartInfo.name, "all");
			}
			else {
				if (!skip_confirm)
					if (!check_confirm("erase partition")) {
						argc -= 2; argv += 2;
						continue;
					}
				get_partition_info(io, name, 0);
			}
			if (!gPartInfo.size) { DBG_LOG("part not exist\n"); argc -= 2; argv += 2; continue; }
			erase_partition(io, gPartInfo.name);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "erase_all")) {
			if (!check_confirm("erase all")) {
				argc -= 1; argv += 1;
				continue;
			}
			erase_partition(io, "all");
			argc -= 1; argv += 1;

		}
		else if (!strcmp(str2[1], "write_part") || !strcmp(str2[1], "w")) {
			const char *fn; FILE *fi;
			const char *name = str2[2];
			if (argcount <= 3) { DBG_LOG("write_part part_name/part_id FILE\n"); argc = 1; continue; }
			fn = str2[3];
			fi = fopen(fn, "r");
			if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= 3; argv += 3; continue; }
			else fclose(fi);
			if (!skip_confirm)
				if (!check_confirm("write partition")) {
					argc -= 3; argv += 3;
					continue;
				}
			get_partition_info(io, name, 0);
			if (!gPartInfo.size) { DBG_LOG("part not exist\n"); argc -= 3; argv += 3; continue; }

			load_partition_unify(io, gPartInfo.name, fn, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			argc -= 3; argv += 3;

		}
		else if (!strncmp(str2[1], "write_parts", 11)) {
			if (argcount <= 2) { DBG_LOG("write_parts|write_parts_a|write_parts_b save_location\n"); argc = 1; continue; }
			int force_ab = 0;
			if (!strcmp(str2[1], "write_parts_a")) force_ab = 1;
			else if (!strcmp(str2[1], "write_parts_b")) force_ab = 2;
			if (skip_confirm || check_confirm("write all partitions")) load_partitions(io, str2[2], blk_size ? blk_size : DEFAULT_BLK_SIZE, force_ab);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "w_force")) {
			const char *fn; FILE *fi;
			const char *name = str2[2];
			if (argcount <= 3) { DBG_LOG("w_force part_name/part_id FILE\n"); argc = 1; continue; }
			if (Da_Info.dwStorageType == 0x101) { DBG_LOG("w_force is not allowed on NAND(UBI) devices\n"); argc -= 3; argv += 3; continue; }
			if (!io->part_count) { DBG_LOG("Partition table not available\n"); argc -= 3; argv += 3; continue; }
			fn = str2[3];
			fi = fopen(fn, "r");
			if (fi == NULL) { DBG_LOG("File does not exist.\n"); argc -= 3; argv += 3; continue; }
			else fclose(fi);
			get_partition_info(io, name, 0);
			if (!gPartInfo.size) { DBG_LOG("part not exist\n"); argc -= 3; argv += 3; continue; }

			if (!strncmp(gPartInfo.name, "splloader", 9)) { DBG_LOG("blacklist!\n"); argc -= 3; argv += 3; continue; }
			else if (isdigit(str2[2][0])) load_partition_force(io, atoi(str2[2]) - 1, fn, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			else {
				for (i = 0; i < io->part_count; i++)
					if (!strcmp(gPartInfo.name, (*(io->ptable + i)).name)) {
						load_partition_force(io, i, fn, blk_size ? blk_size : DEFAULT_BLK_SIZE);
						break;
					}
			}
			argc -= 3; argv += 3;

		}
		else if (!strcmp(str2[1], "wof") || !strcmp(str2[1], "wov")) {
			uint64_t offset;
			size_t length;
			uint8_t *src;
			const char *name = str2[2];
			if (argcount <= 4) { DBG_LOG("wof part_name offset FILE\nwov part_name offset VALUE(max is 0xFFFFFFFF)\n"); argc = 1; continue; }
			if (strstr(name, "fixnv") || strstr(name, "runtimenv") || strstr(name, "userdata")) {
				DBG_LOG("blacklist!\n");
				argc -= 4; argv += 4;
				continue;
			}

			offset = str_to_size(str2[3]);
			if (!strcmp(str2[1], "wov")) {
				src = malloc(4);
				if (!src) { DBG_LOG("malloc failed\n"); argc -= 4; argv += 4; continue; }
				length = 4;
				*(uint32_t *)src = strtoul(str2[4], NULL, 0);
			}
			else {
				const char *fn = str2[4];
				src = loadfile(fn, &length, 0);
				if (!src) { DBG_LOG("fopen %s failed\n", fn); argc -= 4; argv += 4; continue; }
			}
			w_mem_to_part_offset(io, name, offset, src, length, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			free(src);
			argc -= 4; argv += 4;

		}
		else if (!strcmp(str2[1], "read_pactime")) {
			read_pactime(io);
			argc -= 1; argv += 1;

		}
		else if (!strcmp(str2[1], "blk_size") || !strcmp(str2[1], "bs")) {
			if (argcount <= 2) { DBG_LOG("blk_size byte\n\tmax is 65535\n"); argc = 1; continue; }
			blk_size = strtol(str2[2], NULL, 0);
#ifndef _MYDEBUG
			blk_size = blk_size < 0 ? 0 :
				blk_size > 0xf800 ? 0xf800 : ((blk_size + 0x7FF) & ~0x7FF);
#else
			blk_size = blk_size < 0 ? 0 : blk_size;
#endif
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "fblk_size") || !strcmp(str2[1], "fbs")) {
			if (argcount <= 2) { DBG_LOG("fblk_size mb\n"); argc = 1; continue; }
			fblk_size = strtoull(str2[2], NULL, 0) * 1024 * 1024;
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "verity")) {
			if (argcount <= 2) { DBG_LOG("verity {0,1}\n"); argc = 1; continue; }
			if (atoi(str2[2])) dm_enable(io, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			else {
				if (!io->part_count) {
					DBG_LOG("Warning: disable dm-verity needs a valid partition table or a write-verification-disabled FDL2\n");
					if (!skip_confirm)
						if (!check_confirm("disable dm-verity")) {
							argc -= 2; argv += 2;
							continue;
						}
				}
				dm_disable(io, blk_size ? blk_size : DEFAULT_BLK_SIZE);
			}
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "set_active")) {
			if (argcount <= 2) { DBG_LOG("set_active {a,b}\n"); argc = 1; continue; }
			set_active(io, str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "firstmode")) {
			if (argcount <= 2) { DBG_LOG("firstmode mode_id\n"); argc = 1; continue; }
			uint8_t *modebuf = malloc(4);
			if (!modebuf) ERR_EXIT("malloc failed\n");
			uint32_t mode = strtol(str2[2], NULL, 0) + 0x53464D00;
			memcpy(modebuf, &mode, 4);
			w_mem_to_part_offset(io, "miscdata", 0x2420, modebuf, 4, 0x1000);
			free(modebuf);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "skip_confirm")) {
			if (argcount <= 2) { DBG_LOG("skip_confirm {0,1}\n"); argc = 1; continue; }
			skip_confirm = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "rawdata")) {
			if (argcount <= 2) { DBG_LOG("rawdata {0,1,2}\n"); argc = 1; continue; }
			Da_Info.bSupportRawData = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "slot")) {
			if (argcount <= 2) { DBG_LOG("slot {0,1,2}\n"); argc = 1; continue; }
			selected_ab = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "chip_uid")) {
			encode_msg_nocpy(io, BSL_CMD_READ_CHIP_UID, 0);
			send_msg(io);
			ret = recv_msg(io);
			if (!ret) ERR_EXIT("timeout reached\n");
			if ((ret = recv_type(io)) != BSL_REP_READ_CHIP_UID) {
				DBG_LOG("unexpected response (0x%04x)\n", ret); argc -= 1; argv += 1; continue;
			}

			DBG_LOG("BSL_REP_READ_CHIP_UID: ");
			print_string(stderr, io->raw_buf + 4, READ16_BE(io->raw_buf + 2));
			argc -= 1; argv += 1;

		}
		else if (!strcmp(str2[1], "disable_transcode")) {
			encode_msg_nocpy(io, BSL_CMD_DISABLE_TRANSCODE, 0);
			if (!send_and_check(io)) io->flags &= ~FLAGS_TRANSCODE;
			argc -= 1; argv += 1;

		}
		else if (!strcmp(str2[1], "transcode")) {
			unsigned a, f;
			if (argcount <= 2) { DBG_LOG("transcode {0,1}\n"); argc = 1; continue; }
			a = atoi(str2[2]);
			if (a >> 1) { DBG_LOG("transcode {0,1}\n"); argc -= 2; argv += 2; continue; }
			f = (io->flags & ~FLAGS_TRANSCODE);
			io->flags = f | (a ? FLAGS_TRANSCODE : 0);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "keep_charge")) {
			if (argcount <= 2) { DBG_LOG("keep_charge {0,1}\n"); argc = 1; continue; }
			keep_charge = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "timeout")) {
			if (argcount <= 2) { DBG_LOG("timeout ms\n"); argc = 1; continue; }
			io->timeout = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "end_data")) {
			if (argcount <= 2) { DBG_LOG("end_data {0,1}\n"); argc = 1; continue; }
			end_data = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (!strcmp(str2[1], "reset")) {
			if (!fdl1_loaded) {
				DBG_LOG("FDL NOT READY\n");
				argc -= 1; argv += 1;
				continue;
			}
			encode_msg_nocpy(io, BSL_CMD_NORMAL_RESET, 0);
			if (!send_and_check(io)) break;

		}
		else if (!strcmp(str2[1], "reboot-recovery")) {
			if (!fdl1_loaded) {
				DBG_LOG("FDL NOT READY\n");
				argc -= 1; argv += 1;
				continue;
			}
			char *miscbuf = malloc(0x800);
			if (!miscbuf) ERR_EXIT("malloc failed\n");
			memset(miscbuf, 0, 0x800);
			strcpy(miscbuf, "boot-recovery");
			w_mem_to_part_offset(io, "misc", 0, (uint8_t *)miscbuf, 0x800, 0x1000);
			free(miscbuf);
			encode_msg_nocpy(io, BSL_CMD_NORMAL_RESET, 0);
			if (!send_and_check(io)) break;

		}
		else if (!strcmp(str2[1], "reboot-fastboot")) {
			if (!fdl1_loaded) {
				DBG_LOG("FDL NOT READY\n");
				argc -= 1; argv += 1;
				continue;
			}
			char *miscbuf = malloc(0x800);
			if (!miscbuf) ERR_EXIT("malloc failed\n");
			memset(miscbuf, 0, 0x800);
			strcpy(miscbuf, "boot-recovery");
			strcpy(miscbuf + 0x40, "recovery\n--fastboot\n");
			w_mem_to_part_offset(io, "misc", 0, (uint8_t *)miscbuf, 0x800, 0x1000);
			free(miscbuf);
			encode_msg_nocpy(io, BSL_CMD_NORMAL_RESET, 0);
			if (!send_and_check(io)) break;

		}
		else if (!strcmp(str2[1], "poweroff")) {
			if (!fdl1_loaded) {
				DBG_LOG("FDL NOT READY\n");
				argc -= 1; argv += 1;
				continue;
			}
			encode_msg_nocpy(io, BSL_CMD_POWER_OFF, 0);
			if (!send_and_check(io)) break;

		}
		else if (!strcmp(str2[1], "verbose")) {
			if (argcount <= 2) { DBG_LOG("verbose {0,1,2}\n"); argc = 1; continue; }
			io->verbose = atoi(str2[2]);
			argc -= 2; argv += 2;

		}
		else if (strlen(str2[1])) {
			print_help();
			argc = 1;
		}
		if (in_quote != -1)
			for (i = 1; i < argcount; i++)
				free(str2[i]);
		free(str2);
		if (command_delay_us) usleep(command_delay_us);
		if (m_bOpened == -1) {
			DBG_LOG("device removed, exiting...\n");
			break;
		}
	}
	spdio_free(io);
	return 0;
}

static int count_ports(void) {
#if USE_LIBUSB
	libusb_device **ports = FindPort(0x4d00);
	int count = 0;
	if (!ports) return 0;
	for (libusb_device **port = ports; *port != NULL; port++) count++;
	libusb_free_device_list(ports, 1);
	return count;
#else
	DWORD *ports = FindPort("SPRD U2S Diag");
	int count = 0;
	if (!ports) return 0;
	for (DWORD *port = ports; *port != 0; port++) count++;
	free(ports);
	return count;
#endif
}

static int wait_for_ports(int wait, int verbose, int target_count) {
#if USE_LIBUSB
	int i;
	if (libusb_init(NULL) < 0) return 0;
	for (i = 0; ; i++) {
		if (!(i % 2)) {
			int found = count_ports();
			if (found >= target_count) break;
		}
		if (i >= wait) return 0;
		if (verbose) {
			int found = count_ports();
			DBG_LOG("Waiting for devices... %.1fs (%d/%d)\n", (float)i / REOPEN_FREQ, found, target_count);
		}
		usleep(1000000 / REOPEN_FREQ);
	}
	libusb_exit(NULL);
#else
	int i;
	for (i = 0; ; i++) {
		if (!(i % 2)) {
			int found = count_ports();
			if (found >= target_count) break;
		}
		if (i >= wait) return 0;
		if (verbose) {
			int found = count_ports();
			DBG_LOG("Waiting for devices... %.1fs (%d/%d)\n", (float)i / REOPEN_FREQ, found, target_count);
		}
		usleep(1000000 / REOPEN_FREQ);
	}
#endif
	return 1;
}

int main(int argc, char **argv) {
	int wait = 30 * REOPEN_FREQ;
	int target_count = 1;
	int verbose = 0;
	char *batch_argv[] = {
		"spd_dump",
		"fdl", "fdl1-sign.bin", "0x5500",
		"fdl", "fdl2-sign.bin", "0x9efffe00",
		"exec",
		"w", "splloader", "splloader.bin",
		"w", "vbmeta", "vbmeta.img",
		"w", "boot", "boot.img",
		"reset"
	};
	int batch_argc = (int)(sizeof(batch_argv) / sizeof(batch_argv[0]));
#if !USE_LIBUSB
	extern DWORD curPort;
	DWORD *ports = NULL;
#else
	extern libusb_device *curPort;
	libusb_device **ports = NULL;
#endif

	while (argc > 1) {
		if (!strcmp(argv[1], "--wait")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			wait = atoi(argv[2]) * REOPEN_FREQ;
			if (argc > 3 && argv[3][0] != '-') {
				target_count = atoi(argv[3]);
				if (target_count <= 0) ERR_EXIT("bad wait count\n");
				argc -= 3; argv += 3;
			}
			else {
				argc -= 2; argv += 2;
			}
		}
		else if (!strcmp(argv[1], "--verbose")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			verbose = atoi(argv[2]);
			argc -= 2; argv += 2;
		}
		else if (strstr(argv[1], "help") || strstr(argv[1], "-h") || strstr(argv[1], "-?")) {
			print_help();
			return 0;
		}
		else {
			print_help();
			return 1;
		}
	}

	if (!wait_for_ports(wait, verbose, target_count)) ERR_EXIT("find port failed\n");
#if USE_LIBUSB
	ports = FindPort(0x4d00);
	if (!ports) ERR_EXIT("find port failed\n");
	for (libusb_device **port = ports; *port != NULL; port++) {
		reset_runtime_state();
		curPort = *port;
		m_bOpened = 0;
		run_batch_sequence(batch_argc, batch_argv, wait, verbose);
		curPort = NULL;
	}
	libusb_free_device_list(ports, 1);
#else
	ports = FindPort("SPRD U2S Diag");
	if (!ports) ERR_EXIT("find port failed\n");
	for (DWORD *port = ports; *port != 0; port++) {
		reset_runtime_state();
		curPort = *port;
		m_bOpened = 0;
		run_batch_sequence(batch_argc, batch_argv, wait, verbose);
		curPort = 0;
	}
	free(ports);
#endif
	return 0;
}
