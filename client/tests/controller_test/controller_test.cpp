#include <stdio.h>
#include <stdint.h>
#include <spice/controller_prot.h>

#ifdef WIN32

#include <windows.h>

#define PIPE_NAME TEXT("\\\\.\\pipe\\SpiceController-%lu")

static HANDLE pipe = INVALID_HANDLE_VALUE;

#else

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

typedef void *LPVOID;
typedef const void *LPCVOID;
typedef unsigned long DWORD;
typedef char TCHAR;

#define PIPE_NAME "/tmp/SpiceController-%lu.uds"

static int sock = -1;

#endif

#define PIPE_NAME_MAX_LEN 256

void write_to_pipe(LPCVOID data, DWORD size)
{
#ifdef WIN32
    DWORD written;
    if (!WriteFile(pipe, data, size, &written, NULL) || written != size) {
        printf("Write to pipe failed %u\n", GetLastError());
    }
#else
    if (send(sock, data, size, 0) != size) {
        printf("send failed, (%d) %s\n", errno, strerror(errno));
    }
#endif
}

void send_init()
{
    ControllerInit msg = {{CONTROLLER_MAGIC, CONTROLLER_VERSION, sizeof(msg)}, 0,
        CONTROLLER_FLAG_EXCLUSIVE};
    write_to_pipe((LPCVOID)&msg, sizeof(msg));
}

void send_msg(uint32_t id)
{
    ControllerMsg msg = {id, sizeof(msg)};
    write_to_pipe((LPCVOID)&msg, sizeof(msg));
}

void send_value(uint32_t id, uint32_t value)
{
    ControllerValue msg = {{id, sizeof(msg)}, value};
    write_to_pipe((LPCVOID)&msg, sizeof(msg));
}

void send_data(uint32_t id, uint8_t* data, size_t data_size)
{
    size_t size = sizeof(ControllerData) + data_size;
    ControllerData* msg = (ControllerData*)malloc(size);
    msg->base.id = id;
    msg->base.size = (uint32_t)size;
    memcpy(msg->data, data, data_size);
    write_to_pipe((LPCVOID)msg, (DWORD)size);
    free(msg);
}

DWORD read_from_pipe(LPVOID data, DWORD size)
{
    DWORD read;
#ifdef WIN32
    if (!ReadFile(pipe, data, size, &read, NULL)) {
        printf("Read from pipe failed %u\n", GetLastError());
    }
#else
    if (read = recv(sock, data, size, 0) && (read == -1 || read == 0)) {
        printf("recv failed, (%d) %s\n", errno, strerror(errno));
    }
#endif
    return read;
}

#define HOST "localhost"
#define PORT 5931
#define SPORT 0
#define PWD ""
#define SECURE_CHANNELS "main,inputs,playback"
#define DISABLED_CHANNELS "playback,record"
#define TITLE "Hello from controller"
#define HOTKEYS "toggle-fullscreen=shift+f1,release-cursor=shift+f2"
#define MENU "0\r4864\rS&end Ctrl+Alt+Del\tCtrl+Alt+End\r0\r\n" \
    "0\r5120\r&Toggle full screen\tShift+F11\r0\r\n" \
    "0\r1\r&Special keys\r4\r\n" \
    "1\r5376\r&Send Shift+F11\r0\r\n" \
    "1\r5632\r&Send Shift+F12\r0\r\n" \
    "1\r5888\r&Send Ctrl+Alt+End\r0\r\n" \
    "0\r1\r-\r1\r\n" \
    "0\r2\rChange CD\r4\r\n" \
    "2\r3\rNo CDs\r0\r\n" \
    "2\r4\r[Eject]\r0\r\n" \
    "0\r5\r-\r1\r\n" \
    "0\r6\rPlay\r0\r\n" \
    "0\r7\rSuspend\r0\r\n" \
    "0\r8\rStop\r0\r\n"

#define TLS_CIPHERS "NONE"
#define CA_FILE "NONE"
#define HOST_SUBJECT "NONE"

int main(int argc, char *argv[])
{
    int spicec_pid = (argc > 1 ? atoi(argv[1]) : 0);
    char* host = (argc > 2 ? argv[2] : (char*)HOST);
    int port = (argc > 3 ? atoi(argv[3]) : PORT);
    TCHAR pipe_name[PIPE_NAME_MAX_LEN];
    ControllerValue msg;
    DWORD read;

#ifdef WIN32
    _snwprintf_s(pipe_name, PIPE_NAME_MAX_LEN, PIPE_NAME_MAX_LEN, PIPE_NAME, spicec_pid);
    printf("Creating Spice controller connection %S\n", pipe_name);
    pipe = CreateFile(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        printf("Could not open pipe %u\n", GetLastError());
        return -1;
    }
#else
    snprintf(pipe_name, PIPE_NAME_MAX_LEN, PIPE_NAME, spicec_pid);
    printf("Creating a controller connection %s\n", pipe_name);
    struct sockaddr_un remote;
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        printf("Could not open socket, (%d) %s\n", errno, strerror(errno));
        return -1;
    }
    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, pipe_name);
    if (connect(sock, (struct sockaddr *)&remote,
                strlen(remote.sun_path) + sizeof(remote.sun_family)) == -1) {
        printf("Socket connect failed, (%d) %s\n", errno, strerror(errno));
        close(sock);
        return -1;
    }
#endif
    send_init();
    printf("Setting Spice parameters\n");
    send_data(CONTROLLER_HOST, (uint8_t*)host, strlen(host) + 1);
    send_value(CONTROLLER_PORT, port);
    send_value(CONTROLLER_SPORT, SPORT);
    send_data(CONTROLLER_PASSWORD, (uint8_t*)PWD, sizeof(PWD));
    //send_data(CONTROLLER_SECURE_CHANNELS, (uint8_t*)SECURE_CHANNELS, sizeof(SECURE_CHANNELS));
    send_data(CONTROLLER_DISABLE_CHANNELS, (uint8_t*)DISABLED_CHANNELS, sizeof(DISABLED_CHANNELS));
    //send_data(CONTROLLER_TLS_CIPHERS, (uint8_t*)TLS_CIPHERS, sizeof(TLS_CIPHERS));
    //send_data(CONTROLLER_CA_FILE, (uint8_t*)CA_FILE, sizeof(CA_FILE));
    //send_data(CONTROLLER_HOST_SUBJECT, (uint8_t*)HOST_SUBJECT, sizeof(HOST_SUBJECT));
    send_data(CONTROLLER_SET_TITLE, (uint8_t*)TITLE, sizeof(TITLE));
    send_data(CONTROLLER_HOTKEYS, (uint8_t*)HOTKEYS, sizeof(HOTKEYS));
    
    send_data(CONTROLLER_CREATE_MENU, (uint8_t*)MENU, sizeof(MENU));

    send_value(CONTROLLER_FULL_SCREEN, /*CONTROLLER_SET_FULL_SCREEN |*/ CONTROLLER_AUTO_DISPLAY_RES);
    printf("Show...\n");
    getchar();
    send_msg(CONTROLLER_SHOW);

    printf("Connect...\n");
    getchar();
    send_msg(CONTROLLER_CONNECT);

    printf("Hide...\n");
    getchar();
    send_msg(CONTROLLER_HIDE);

    printf("Show...\n");
    getchar();
    send_msg(CONTROLLER_SHOW);

    //send_msg(CONTROLLER_DELETE_MENU);
    //send_msg(CONTROLLER_HIDE);
    while ((read = read_from_pipe(&msg, sizeof(msg))) == sizeof(msg)) {
        printf("Received id %u, size %u, value %u\n", msg.base.id, msg.base.size, msg.value);
    }
    printf("Press <Enter> to close connection\n");
    getchar();
#ifdef WIN32
    CloseHandle(pipe); 
#else
    close(sock);
#endif
    return 0;
}
