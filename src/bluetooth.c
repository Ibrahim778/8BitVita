#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/bt.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/sblaimgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2/touch.h>
#include <string.h>
#include <psp2/motion.h>
#include <taihen.h>
#include "common.h"

typedef struct 
{
	unsigned int mac0;
	unsigned int mac1;
	int connected;
	int patch;
} ControllerInfo;

#define MICROSOFT_VID 0x45E
#define XBOX_CONTROLLER_PID 0x2E0

#define CONTROLLER_ANALOG_THRESHOLD 4
#define DEADZONE_ANALOG(a) ((a) > 127 - CONTROLLER_ANALOG_THRESHOLD && (a) < 127 + CONTROLLER_ANALOG_THRESHOLD ? 127 : (a))
#define abs(x) (((x) < 0) ? -(x) : (x))

static SceUID bt_mempool_uid = -1;
static SceUID bt_thread_uid = -1;
static SceUID bt_cb_uid = -1;
static int bt_thread_run = 1;

static int lt_rt_swap = 0;
static int ignoreHook = 0;

static int lastPID = -1, lastVID = -1;

static ControllerInfo controllers[CONTROLLER_COUNT];
static char current_recieved_input[CONTROLLER_COUNT][0x12];
static char battery_info[CONTROLLER_COUNT][0x12];

static char original_input[CONTROLLER_COUNT];

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
static int (* ksceKernelSysrootCheckModelCapability)(int capability) = NULL;

#pragma region Definitions

#define UNBIND_FUNC_HOOK(name) \
	do { \
		if (name##_hook_uid > 0) { \
			taiHookReleaseForKernel(name##_hook_uid, name##_ref); \
		} \
	} while(0)

#define DECL_FUNC_HOOK(name, ...) \
	static tai_hook_ref_t name##_ref; \
	static SceUID name##_hook_uid = -1; \
	static int name##_hook_func(__VA_ARGS__)

#define DECL_FUNC_HOOK_WITH_TYPE(name, type, ...) \
	static tai_hook_ref_t name##_ref; \
	static SceUID name##_hook_uid = -1; \
	static type name##_hook_func(__VA_ARGS__)

#define DECL_FUNC_HOOK_PATCH_CTRL(type, name, triggers, logic) \
	DECL_FUNC_HOOK(SceCtrl_##name, int port, SceCtrlData *pad_data, int count) \
	{ \
		int ret = TAI_CONTINUE(int, SceCtrl_ ##name##_ref, port, pad_data, count, triggers); \
		if (ret >= 0 && controllers[port].connected) \
			patch_ctrl_data_all_##type(port, pad_data, count, triggers, logic); \
		return ret; \
	}

#define BIND_FUNC_OFFSET_HOOK(name, pid, modid, segidx, offset, thumb) \
	name##_hook_uid = taiHookFunctionOffsetForKernel((pid), \
		&name##_ref, (modid), (segidx), (offset), thumb, name##_hook_func)

#define BIND_FUNC_EXPORT_HOOK(name, pid, module, lib_nid, func_nid) \
	name##_hook_uid = taiHookFunctionExportForKernel((pid), \
		&name##_ref, (module), (lib_nid), (func_nid), name##_hook_func)
#pragma endregion Definitions
#pragma region Generics

int findPort(unsigned int mac0, unsigned int mac1)
{
	for (int i = 0; i < CONTROLLER_COUNT; i++)
	{
		if(controllers[i].mac0 == mac0 && controllers[i].mac1 == mac1) 
		{
			return i;
		}
	}
	return -1;
}

int findFreePort()
{	
	SceCtrlPortInfo info;
	ksceCtrlGetControllerPortInfo(&info);
	if(ksceKernelSysrootCheckModelCapability(1))
	{		
		for (int i = 1; i < CONTROLLER_COUNT; i++)
		{
			if(info.port[i] != SCE_CTRL_TYPE_DS4 && info.port[i] != SCE_CTRL_TYPE_DS3) 
			{
				return i;
			}
		}
	}
	else
	{
		if(info.port[1] == SCE_CTRL_TYPE_DS3 || info.port[1] == SCE_CTRL_TYPE_DS4) return -1;
		else return 1;
	}
	return -1;
}

int checkFileExist(const char *file)
{
	int fd = ksceIoOpen(file, SCE_O_RDONLY, 0);
	if(fd < 0) return 0;
	ksceIoClose(fd);
	return 1;
}

int checkDirExist(const char *file)
{
	int fd = ksceIoDopen(file);
	if(fd < 0) return 0;
	ksceIoDclose(fd);
	return 1;
}

void createFile(const char *file)
{
	int fd = ksceIoOpen(file, SCE_O_WRONLY | SCE_O_CREAT, 0777);
	ksceIoClose(fd);
}

static inline void controller_input_reset(void)
{
	for (int i = 0; i < CONTROLLER_COUNT; i++)
	{
		memset(current_recieved_input[i], 0, sizeof current_recieved_input[i]);
	}
}

//Returns 1 if any controller is connected else 0
int getConnectionStatus()
{
	int found = 0;
	for (int i = 1; i < CONTROLLER_COUNT; i++)
	{
		if(controllers[i].connected) 
		{
			found = 1;
			break;
		}
	}
	return found;
}

static int is_controller(const unsigned short vid_pid[2])
{
	lastVID = vid_pid[0];
	lastPID = vid_pid[1];
	return (vid_pid[0] == MICROSOFT_VID) && (vid_pid[1] == XBOX_CONTROLLER_PID);
}

static inline void *mempool_alloc(unsigned int size)
{
	return ksceKernelAllocHeapMemory(bt_mempool_uid, size);
}

static inline void mempool_free(void *ptr)
{
	ksceKernelFreeHeapMemory(bt_mempool_uid, ptr);
}
#pragma endregion Generics
#pragma region Exports
//Exports

//Get weather the triggers or bumpers are swaped (bool)
int GetSwapStatus()
{
	return lt_rt_swap;
}

//Set weather the triggers or bumpers are swaped (bool). Returns 0 if success < 0 on error.
int SetSwapStatus(int *status)
{
	int setRes = ksceKernelMemcpyUserToKernel(&lt_rt_swap, (uintptr_t)status, sizeof(int));
	if(setRes < 0) return setRes;
	if(!lt_rt_swap)
	{	
		if(checkFileExist(ConfigPath))
		{
			ksceIoRemove(ConfigPath);
			return 0;
		}
	}
	if(lt_rt_swap)
	{
		if(!checkFileExist(ConfigPath))
		{
			if(!checkDirExist("ux0:data/X1Vita"))
				ksceIoMkdir("ux0:data/X1Vita", SCE_S_IWUSR | SCE_S_IRUSR);
			createFile(ConfigPath);	
			return 0;
		}
	}
	return 0;
}

//Get PID and VID of last request
int GetPidVid(int *vid, int *pid)
{
	int ret;
	ret = ksceKernelMemcpyKernelToUser((uintptr_t)vid, &lastVID, sizeof(lastVID));
	if (ret < 0) return ret;
	return ksceKernelMemcpyKernelToUser((uintptr_t)pid, &lastPID, sizeof(lastPID));
}

//Get Port Info
int GetPortBuff(const char* buff)
{
	return ksceKernelMemcpyKernelToUser((uintptr_t)buff, original_input, 5);
}

//Get current data recieved from the connected bluetooth device
int GetBuff(int port, const char* buff)
{
	//We use memcpy instead of strncpy so it copies the whole thing if not values will be cut off.
	return ksceKernelMemcpyKernelToUser((uintptr_t)buff, &current_recieved_input[port], 0x12);
} 
#pragma endregion Exports
#pragma region Hooks
DECL_FUNC_HOOK(SceBt_sub_22999C8, void *dev_base_ptr, int r1)
{
	unsigned int flags = *(unsigned int *)(r1 + 4);

	if (dev_base_ptr && !(flags & 2)) {
		const void *dev_info = *(const void **)(dev_base_ptr + 0x14A4);
		const unsigned short *vid_pid = (const unsigned short *)(dev_info + 0x28);

		if (is_controller(vid_pid)) {
			unsigned int *v8_ptr = (unsigned int *)(*(unsigned int *)dev_base_ptr + 8);

			/*
			 * We need to enable the following bits in order to make the Vita
			 * accept the new connection, otherwise it will refuse it.
			 */
			*v8_ptr |= 0x11000;
		}
	}

	return TAI_CONTINUE(int, SceBt_sub_22999C8_ref, dev_base_ptr, r1);
}

DECL_FUNC_HOOK_WITH_TYPE(SceBt_sub_22947E4, void *, unsigned int r0, unsigned int r1, unsigned long long r2)
{
	void *ret = TAI_CONTINUE(void *, r0, r1, r2);
	if(ret)
	{
		*(unsigned int *)(ret + 0x24) |= 0x1000;
	}
	return ret;
}

DECL_FUNC_HOOK(SceCtrl_ksceCtrlGetControllerPortInfo, SceCtrlPortInfo *info)
{
	int ret = TAI_CONTINUE(int, SceCtrl_ksceCtrlGetControllerPortInfo_ref, info);

	if(!ignoreHook)
	{
		if(getConnectionStatus() && ksceKernelSysrootCheckModelCapability(1))
			info->port[0] = getConnectionStatus() ? SCE_CTRL_TYPE_VIRT : SCE_CTRL_TYPE_UNPAIRED;
		
		original_input[0] = info->port[0];

		for (int i = 1; i < CONTROLLER_COUNT; i++)
		{
			if(controllers[i].connected) info->port[i] = SCE_CTRL_TYPE_DS4;
			original_input[i] = info->port[i];
		}
	}
	return ret;
}
#pragma endregion Hooks

#pragma region Functions
static int controller_send_report(unsigned int mac0, unsigned int mac1, uint8_t flags, uint8_t report, size_t len, const void *data)
{
	SceBtHidRequest *req;
	unsigned char *buf;

	req = mempool_alloc(sizeof(*req));
	if (!req) {
		return -1;
	}

	if ((buf = mempool_alloc((len + 1) * sizeof(*buf))) == NULL) {
		return -1;
	}

	buf[0] = report;
	memcpy(buf + 1, data, len);

	memset(req, 0, sizeof(*req));
	req->type = 1; // 0xA2 -> type = 1
	req->buffer = buf;
	req->length = len + 1;
	req->next = req;

	int res = ksceBtHidTransfer(mac0, mac1, req);
	if(res < 0) return -1;
	mempool_free(buf);
	mempool_free(req);

	return 0;
}

static int controller_send_0x11_report(unsigned int mac0, unsigned int mac1)
{
	unsigned char data[] = {
		0x80,
		0x0F,
		0x00,
		0x00,
		0x00,
		0x00, 
		0x00, 
		0x00, 
		0x00, 
		0x00, 
		0x00,
		0x00,
	};

	if (controller_send_report(mac0, mac1, 0, 0x11, sizeof(data), data) < 0) {
		return -1;
	}

	return 0;
}
static void enqueue_read_request(unsigned int mac0, unsigned int mac1, SceBtHidRequest *request, unsigned char *buffer, unsigned int length)
{
	memset(request, 0, sizeof(*request));
	memset(buffer, 0, length);

	request->type = 0;
	request->buffer = buffer;
	request->length = length;
	request->next = request;

	ksceBtHidTransfer(mac0, mac1, request);
}
static int bt_cb_func(int notifyId, int notifyCount, int notifyArg, void *common)
{

	static SceBtHidRequest hid_request;
	static unsigned char recv_buff[0x100];
	while (1) {
		int ret;
		SceBtEvent hid_event;

		memset(&hid_event, 0, sizeof(hid_event));

		do {
			ret = ksceBtReadEvent(&hid_event, 1);
		} while (ret == SCE_BT_ERROR_CB_OVERFLOW);

		if (ret <= 0) {
			break;
		}

		unsigned short vid_pid[2];
		ksceBtGetVidPid(hid_event.mac0, hid_event.mac1, vid_pid);
		if(!is_controller(vid_pid))
			continue;

		switch (hid_event.id) 
		{

			case 0x05: 
			{ /* Connection accepted event */ 
				if (is_controller(vid_pid)) 
				{
					int port = findFreePort();
					if(port < 0) 
					{
						ksceBtStartDisconnect(hid_event.mac0, hid_event.mac1);
						break;
					}
					controllers[port].connected = 1;
					if(port == 1) controllers[0].connected = 1;
					controllers[port].mac0 = hid_event.mac0;
					controllers[port].mac1 = hid_event.mac1;
					controllers[port].patch = 1;
					if(port == 1) controllers[0].patch = 1; 
					controller_send_0x11_report(hid_event.mac0, hid_event.mac1);
				}
				break;
			}

			case 0x06:
			{
				int port = findPort(hid_event.mac0, hid_event.mac1);
				controllers[port].connected = 0;
				if(port == 1) controllers[0].connected = 0;
				break;	
			} 
	
			case 0x0A:
			{
				int port = findPort(hid_event.mac0, hid_event.mac1);
				if(recv_buff[0] != 0x4)
				{
					memcpy(current_recieved_input[port], recv_buff, 0x11);
					if(port == 1)
					{
						memcpy(current_recieved_input[0], recv_buff, 0x11);
					}
				}
				else memcpy(battery_info[port], recv_buff, 0x11);
				enqueue_read_request(hid_event.mac0, hid_event.mac1, &hid_request, recv_buff, sizeof(recv_buff));
				break;
			}

			case 0x0B:
				enqueue_read_request(hid_event.mac0, hid_event.mac1, &hid_request, recv_buff, sizeof(recv_buff));
				break;
		}
	}

	return 0;
}
static int controllervita_bt_thread(SceSize args, void *argp)
{
	bt_cb_uid = ksceKernelCreateCallback("8bitvita_bt_callback", 0, bt_cb_func, NULL);

	ksceBtRegisterCallback(bt_cb_uid, 0, 0xFFFFFFFF, 0xFFFFFFFF);


	while (bt_thread_run) 
	{
		ksceKernelDelayThreadCB(200 * 1000);
	}

	for (int i = 0; i < CONTROLLER_COUNT; i++)
	{
		if(controllers[i].connected) ksceBtStartDisconnect(controllers[i].mac0, controllers[i].mac1);
	}
	
	ksceBtUnregisterCallback(bt_cb_uid);

	ksceKernelDeleteCallback(bt_cb_uid);

	return 0;
}
static void patch_ctrl_data(SceCtrlData *pad_data, int triggers, int port, int isPositive)
{
	if(!controllers[port].patch) return;
	int leftX = 0x80;
	int leftY = 0x80;
	int rightX = 0x80;
	int rightY = 0x80;
	int joyStickMoved = 0;
	unsigned int buttons = 0;

	int lt = ((current_recieved_input[port][9] + (255 * current_recieved_input[port][10])) / 4);
	int rt = ((current_recieved_input[port][11] + (255 * current_recieved_input[port][12])) / 4);

	//Xbox button
	if(current_recieved_input[port][0] & 0x02) 
	{
		if(current_recieved_input[port][1] & 0x01)
		{
			buttons |= SCE_CTRL_PSBUTTON;
			ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 8);
		}
	} //Any other button call
	else if(current_recieved_input[port][0] & 0x1)
	{
		
		//DPad
		switch (current_recieved_input[port][13])
		{
			case 0x1:
				buttons |= SCE_CTRL_UP;
				break;
			case 0x2:
				buttons |= SCE_CTRL_UP;
				buttons |= SCE_CTRL_RIGHT;
				break;
			case 0x3:
				buttons |= SCE_CTRL_RIGHT;
				break;
			case 0x4:
				buttons |= SCE_CTRL_RIGHT;
				buttons |= SCE_CTRL_DOWN;
				break;
			case 0x5:
				buttons |= SCE_CTRL_DOWN;
				break;
			case 0x6:
				buttons |= SCE_CTRL_DOWN;
				buttons |= SCE_CTRL_LEFT;
				break;
			case 0x7:
				buttons |= SCE_CTRL_LEFT;
				break;
			case 0x8:
				buttons |= SCE_CTRL_LEFT;
				buttons |= SCE_CTRL_UP;
				break;
		}

		//RB LB and ABXY. For some reason the buttons aren't or'ed together when pushed separetly which is why we need to do it like this.
		switch (current_recieved_input[port][14])
		{
			case 0x1:
				buttons |= SCE_CTRL_CROSS;
				break;
			case 0x2:
				buttons |= SCE_CTRL_CIRCLE;
				break;
			case 0x8:
				buttons |= SCE_CTRL_SQUARE;
				break;
			case 0x10:
				buttons |= SCE_CTRL_TRIANGLE;
				break;
			case 0x80:
				//RB
				if(!lt_rt_swap)
				{
					if(triggers) buttons |= SCE_CTRL_R1;
					else buttons |= SCE_CTRL_RTRIGGER;
				}
				else
				{
					pad_data->rt = 255;
				}
				break;
			case 0x40:
				//LB
				if(!lt_rt_swap)
				{
					if(triggers) buttons |= SCE_CTRL_L1;
					else buttons |= SCE_CTRL_LTRIGGER;
				}
				else
				{
					pad_data->lt = 255;
				}
				break;
			default:
				{
					if(current_recieved_input[port][14] & 0x1) buttons |= SCE_CTRL_CROSS;
					if(current_recieved_input[port][14] & 0x2) buttons |= SCE_CTRL_CIRCLE;
					if(current_recieved_input[port][14] & 0x8) buttons |= SCE_CTRL_SQUARE;
					if(current_recieved_input[port][14] & 0x10) buttons |= SCE_CTRL_TRIANGLE;		
					if(current_recieved_input[port][14] & 0x80)
					{
						if(!lt_rt_swap)
						{
							if(triggers) buttons |= SCE_CTRL_R1;
							else buttons |= SCE_CTRL_RTRIGGER;
						}
						else
						{
							pad_data->rt = 255;
						}
					}
					if(current_recieved_input[port][14] & 0x40) 
					{
						if(!lt_rt_swap)
						{
							if(triggers) buttons |= SCE_CTRL_L1;
							else buttons |= SCE_CTRL_LTRIGGER;
						}
						else
						{
							pad_data->lt = 255;
						}
					}
					break;
				}
		}

		//Start
		//if(current_recieved_input[port][15] == 0x8) buttons |= SCE_CTRL_START;
		//Select
		if(current_recieved_input[port][16] == 0x1) buttons |= SCE_CTRL_SELECT;

		//R3 L3 Start and Xbox button on newer firmwares controllers
		switch (current_recieved_input[port][15])
		{
			case 0x10:
			{
				buttons |= SCE_CTRL_PSBUTTON;
				ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 8);
				break;
			}
			case 0x8:
				buttons |= SCE_CTRL_START;
				break;
			case 0x40:
				buttons |= SCE_CTRL_R3;
				break;
			case 0x20:
				buttons |= SCE_CTRL_L3;

			default:
				if(current_recieved_input[port][15] & 0x8) 
				{
					buttons |= SCE_CTRL_START;
				}
				if(current_recieved_input[port][15] & 0x40) 
				{
					buttons |= SCE_CTRL_R3;
				}
				if(current_recieved_input[port][15] & 0x20) 
				{
					buttons |= SCE_CTRL_L3;
				}
				if(current_recieved_input[port][15] & 0x10)
				{
					buttons |= SCE_CTRL_PSBUTTON;
					ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 8);
				}
				break;
		}

		//Joysticks
		//Left Joystick X
		leftX = DEADZONE_ANALOG((current_recieved_input[port][2]));
		//Left Joystick Y
		leftY = DEADZONE_ANALOG((current_recieved_input[port][4]));

		//Right Joystick X
		rightX = DEADZONE_ANALOG((current_recieved_input[port][6]));
		//Right Joystick Y
		rightY = DEADZONE_ANALOG((current_recieved_input[port][8]));

		if(leftX != 128 && leftY != 128)
			joyStickMoved = 1;

	}

	//LT RT
	if(lt > 10)
	{
		if(!lt_rt_swap)
		{
			// if triggers = true, ltrigger = L2 & L1 = L1. Else Ltigger = L1 and LTRIGGER = L2
			if(triggers) buttons |= SCE_CTRL_LTRIGGER;
			else buttons |= SCE_CTRL_L1;
		}
		else
		{
			if(triggers) buttons |= SCE_CTRL_L1;
			else buttons |= SCE_CTRL_LTRIGGER;
		}
	}
	if(rt > 10)
	{
		if(!lt_rt_swap)
		{
			if(triggers) buttons |= SCE_CTRL_RTRIGGER;
			else buttons |= SCE_CTRL_R1;
		}
		else
		{
			if(triggers) buttons |= SCE_CTRL_R1;
			else buttons |= SCE_CTRL_RTRIGGER;
		}
	}

	if(!lt_rt_swap)
	{
		pad_data->lt = lt;
		pad_data->rt = rt;
	}
	//Joysticks
	if(joyStickMoved)
	{	
		pad_data->ry = rightY;
		pad_data->rx = rightX;
		pad_data->lx = leftX;
		pad_data->ly = leftY;
	}
	
	pad_data->buttons |= buttons;
	if(!isPositive) pad_data->buttons = 0xFFFFFFFF - pad_data->buttons;
	if(buttons != 0 || joyStickMoved) ksceKernelPowerTick(0);
}
static void patch_ctrl_data_all_kernel(int port, SceCtrlData *pad_data, int count, int triggers, int isPositive)
{
	unsigned int i;

	for (i = 0; i < count; i++, pad_data++)
		patch_ctrl_data(pad_data, triggers, port, isPositive);
}
#pragma endregion Functions

#pragma region CtrlHooks
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlPeekBufferPositive, 0, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlReadBufferPositive, 0, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlPeekBufferNegative, 0, 0)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlReadBufferNegative, 0, 0)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlPeekBufferPositiveExt, 0, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlReadBufferPositiveExt, 0, 1)

DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlPeekBufferPositive2, 1, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlReadBufferPositive2, 1, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlPeekBufferNegative2, 1, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlReadBufferNegative2, 1, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlPeekBufferPositiveExt2, 1, 1)
DECL_FUNC_HOOK_PATCH_CTRL(kernel, ksceCtrlReadBufferPositiveExt2, 1, 1)
//0xFFFFFFFF 
#pragma endregion CtrlHooks

void _start() __attribute__ ((weak, alias ("module_start")));

int module_start(SceSize argc, const void *args)
{
	module_get_export_func(KERNEL_PID, "SceSysmem", TAI_ANY_LIBRARY, 0x8AA268D6, (uintptr_t *)&ksceKernelSysrootCheckModelCapability);
	ignoreHook = !ksceKernelSysrootCheckModelCapability(1);
	memset(controllers, 0, sizeof(controllers));
	int ret;
	tai_module_info_t SceBt_modinfo;

	SceBt_modinfo.size = sizeof(SceBt_modinfo);
	ret = taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &SceBt_modinfo);
	if (ret < 0) {
		goto error_find_scebt;
	}

	tai_module_info_t SceCtrl_modinfo;
	SceCtrl_modinfo.size = sizeof(SceCtrl_modinfo);

	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceCtrl", &SceCtrl_modinfo) < 0) {
		ksceDebugPrintf("Error finding SceBt module\n");
		return SCE_KERNEL_START_FAILED;
		
	}

	#pragma region  BindHooks
	BIND_FUNC_EXPORT_HOOK(SceCtrl_ksceCtrlPeekBufferPositive, KERNEL_PID, "SceCtrl", TAI_ANY_LIBRARY, 0xEA1D3A34);
    BIND_FUNC_EXPORT_HOOK(SceCtrl_ksceCtrlReadBufferPositive, KERNEL_PID, "SceCtrl", TAI_ANY_LIBRARY, 0x9B96A1AA);
    BIND_FUNC_EXPORT_HOOK(SceCtrl_ksceCtrlPeekBufferNegative, KERNEL_PID, "SceCtrl", TAI_ANY_LIBRARY, 0x19895843);
    BIND_FUNC_EXPORT_HOOK(SceCtrl_ksceCtrlReadBufferNegative, KERNEL_PID, "SceCtrl", TAI_ANY_LIBRARY, 0x8D4E0DD1);
	BIND_FUNC_EXPORT_HOOK(SceCtrl_ksceCtrlGetControllerPortInfo, KERNEL_PID, "SceCtrl", TAI_ANY_LIBRARY, 0xF11D0D30);
	BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlPeekBufferPositiveExt, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x3928, 1);
    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlReadBufferPositiveExt, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x3BCC, 1);

    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlPeekBufferPositive2, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x3EF8, 1);
    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlReadBufferPositive2, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x449C, 1);
    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlPeekBufferNegative2, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x41C8, 1);
    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlReadBufferNegative2, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x47F0, 1);
    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlPeekBufferPositiveExt2, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x4B48, 1);
    BIND_FUNC_OFFSET_HOOK(SceCtrl_ksceCtrlReadBufferPositiveExt2, KERNEL_PID, SceCtrl_modinfo.modid, 0, 0x4E14, 1);
	if(ksceSblAimgrIsDolce())
	{
		//BIND_FUNC_OFFSET_HOOK(SceBt_sub_22947E4, KERNEL_PID, SceBt_modinfo.modid, 0, 0x22947E4 - 0x2280000, 1);
		//BIND_FUNC_OFFSET_HOOK(SceBt_sub_22999C8, KERNEL_PID, SceBt_modinfo.modid, 0, 0x22999C8 - 0x2280000, 1);
	}
	#pragma endregion  BindHooks

	SceKernelHeapCreateOpt opt;
	opt.size = 0x1C;
	opt.uselock = 0x100;
	opt.field_8 = 0x10000;
	opt.field_C = 0;
	opt.field_14 = 0;
	opt.field_18 = 0;

	bt_mempool_uid = ksceKernelCreateHeap("8bitvita_mempool", 0x100, &opt);

	bt_thread_uid = ksceKernelCreateThread("8bitvita_bt_thread", controllervita_bt_thread,
		0x3C, 0x1000, 0, 0x10000, 0);
	ksceKernelStartThread(bt_thread_uid, 0, NULL);

	lt_rt_swap = checkFileExist(ConfigPath);

	return SCE_KERNEL_START_SUCCESS;

error_find_scebt:
	return SCE_KERNEL_START_FAILED;
}
int module_stop(SceSize argc, const void *args)
{
	SceUInt timeout = 0xFFFFFFFF;

	if (bt_thread_uid > 0) {
		bt_thread_run = 0;
		ksceKernelWaitThreadEnd(bt_thread_uid, NULL, &timeout);
		ksceKernelDeleteThread(bt_thread_uid);
	}

	if (bt_mempool_uid > 0) {
		ksceKernelDeleteHeap(bt_mempool_uid);
	}
	#pragma region UnbindHooks
	if(ksceSblAimgrIsDolce())
	{
		//UNBIND_FUNC_HOOK(SceBt_sub_22947E4);
		//UNBIND_FUNC_HOOK(SceBt_sub_22999C8);
	}
	UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlPeekBufferPositive);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlReadBufferPositive);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlPeekBufferNegative);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlReadBufferNegative);

	UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlPeekBufferPositiveExt);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlReadBufferPositiveExt);
	UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlGetControllerPortInfo);

    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlPeekBufferPositive2);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlReadBufferPositive2);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlPeekBufferNegative2);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlReadBufferNegative2);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlPeekBufferPositiveExt2);
    UNBIND_FUNC_HOOK(SceCtrl_ksceCtrlReadBufferPositiveExt2);
	#pragma endregion UnbindHooks


	return SCE_KERNEL_STOP_SUCCESS;
}
