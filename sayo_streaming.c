#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <libusb-1.0/libusb.h>

#define VID 0x8089
#define PID 0x0009
#define DEV_WIDTH 160
#define DEV_HEIGHT 80
#define EP_OUT 0x03

int rgb_size = DEV_WIDTH * DEV_HEIGHT * 3;
unsigned char *global_rgb_buffer;
pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
int pipe_fd = -1;

FILE *global_ffmpeg_pipe = NULL;
libusb_device_handle *global_dev_handle = NULL;
libusb_context *global_ctx = NULL;

void clean_on_ex(int signum) {
    printf("\nCTRL C clean \n");

    if (global_ffmpeg_pipe != NULL) {
        pclose(global_ffmpeg_pipe);
    }

    system("pkill -9 -f 'ffmpeg.*rawvideo'");
    system("pkill -9 -f 'ffmpeg.*video50'");

    if (global_dev_handle != NULL) {
        libusb_release_interface(global_dev_handle, 1);
        libusb_attach_kernel_driver(global_dev_handle, 1);
        libusb_close(global_dev_handle);
    }

    _Exit(0);
}

void* video_receiver_thread(void* arg) {
    unsigned char *local_buffer = malloc(rgb_size);
    while (1) {
        int bytes_read = 0;
        while (bytes_read < rgb_size) {
            int r = read(pipe_fd, local_buffer + bytes_read, rgb_size - bytes_read);
            if (r <= 0) {
                break;
            }
            bytes_read += r;
        }

        if (bytes_read == rgb_size) {
            pthread_mutex_lock(&frame_mutex);
            memcpy(global_rgb_buffer, local_buffer, rgb_size);
            pthread_mutex_unlock(&frame_mutex);
        }
    }
    free(local_buffer);
    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, clean_on_ex);

    int actual_len;
    char *vid = NULL;
    char arg[17];
    int has_p = 0;
    int has_l = 0;
    int opt;
    while ((opt = getopt(argc, argv, "p:lh")) != -1) {
        if (opt == 'p') {
            vid = optarg;
            has_p = 1;
        }
        else if(opt == 'h'){
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -p <path>  Stream directly from a file (e.g., mp4, avi, gif, png, jpg)\n");
            printf("  -l         Loop the video\n");
            printf("  -h         Show this help message and exit\n\n");
            printf("If no options are provided, the program streams from /dev/video50 by default.\n");
            return 0;
        }
        else if(opt == 'l') {
            strcpy(arg, "-stream_loop -1");
            has_l = 1; 
        }
        else {
            return 1;
        }
    }
    
    if (libusb_init(&global_ctx) < 0) return 1;
    global_dev_handle = libusb_open_device_with_vid_pid(global_ctx, VID, PID);
    if (!global_dev_handle) {
        printf("SayoDevice not found.\n");
        libusb_exit(global_ctx);
        return 1;
    }

    if (libusb_kernel_driver_active(global_dev_handle, 1) == 1) {
        libusb_detach_kernel_driver(global_dev_handle, 1);
    }
    libusb_claim_interface(global_dev_handle, 1);

    char command_buff[512];
    if (has_l == 1 && has_p == 0) {
        printf("Cannot loop video (-l) because no input file was specified (-p). \n");
        return 1;
    }
    if (vid != NULL) {
        char last_s[4];
        strcpy(last_s, vid + strlen(vid) - 3);
        if(strcmp(last_s, ".mp4") || strcmp(last_s, ".avi") || strcmp(last_s, ".gif"))
            snprintf(command_buff, sizeof(command_buff), "ffmpeg %s -re -i %s -vf \"scale=160:80\" -f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null", arg, vid);
        else if (strcmp(last_s, ".png") || strcmp(last_s, ".jpg"))
            snprintf(command_buff, sizeof(command_buff), "ffmpeg -i %s -vf \"scale=160:80\" -f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null", vid);
        else{
            printf("supports only .mp4, .avi, .gif, .png, .jpg");
            return 1;
        }
    } else {
        snprintf(command_buff, sizeof(command_buff), "ffmpeg -fflags nobuffer -flags low_delay -f v4l2 -i /dev/video50 -vf \"scale=160:80\" -f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null");
    }

    global_ffmpeg_pipe = popen(command_buff, "r");
    if (!global_ffmpeg_pipe) {
        printf("Failed to start FFmpeg pipe\n");
        libusb_release_interface(global_dev_handle, 1);
        libusb_close(global_dev_handle);
        libusb_exit(global_ctx);
        return 1;
    }
    pipe_fd = fileno(global_ffmpeg_pipe);

    global_rgb_buffer = malloc(rgb_size);
    memset(global_rgb_buffer, 0, rgb_size);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, video_receiver_thread, NULL);
    pthread_detach(thread_id);

    unsigned char rgb565[DEV_WIDTH * DEV_HEIGHT * 2];
    unsigned char local_rgb[DEV_WIDTH * DEV_HEIGHT * 3];
    unsigned char pack1024[1024];

    printf("USB streaming has started.\n");

    while (1) {
        pthread_mutex_lock(&frame_mutex);
        memcpy(local_rgb, global_rgb_buffer, rgb_size);
        pthread_mutex_unlock(&frame_mutex);

        int buf_idx = 0;
        for (int i = 0; i < rgb_size; i += 3) {
            unsigned char b = local_rgb[i];
            unsigned char g = local_rgb[i + 1];
            unsigned char r = local_rgb[i + 2];

            unsigned short bgr565_val = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
            rgb565[buf_idx] = bgr565_val & 0xFF;
            rgb565[buf_idx + 1] = (bgr565_val >> 8) & 0xFF;
            buf_idx += 2;
        }

        memset(pack1024, 0, 1024);
        pack1024[12] = 0x01; 
        
        int len_init = 1 + 4; 
        int total_len_init = len_init + 4; 
        
        pack1024[0] = 0x22; 
        pack1024[1] = 0x04; 
        pack1024[4] = total_len_init & 0xFF; 
        pack1024[5] = (total_len_init >> 8) & 0xFF; 
        pack1024[6] = 0x3E;

        unsigned int hash_init = 0;
        int target_len_init = total_len_init + 4;
        if (target_len_init % 2 != 0) target_len_init++;
        
        for (int i = 0; i < target_len_init; i += 2) {
            unsigned short word = (unsigned short)pack1024[i] | ((unsigned short)pack1024[i + 1] << 8);
            hash_init += word;
        }
        pack1024[2] = hash_init & 0xFF; 
        pack1024[3] = (hash_init >> 8) & 0xFF; 
        
        libusb_bulk_transfer(global_dev_handle, EP_OUT, pack1024, 1024, &actual_len, 1000);
        //usleep(1000); 

        // streaming 
        // 0x3E: 22 04 65 04 05 00 3E 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00
        // 0x25: 22 04 24 62 FC 03 25 00 00 00 00 00 E3 18 E3 18 E3 18 E3 18 E3 18 E3 18
        for (int offset = 0; offset < sizeof(rgb565); offset += 1012) {
            int chunk = (sizeof(rgb565) - offset) > 1012 ? 1012 : (sizeof(rgb565) - offset);
            memset(pack1024, 0, 1024);

            pack1024[8]  = (unsigned char)(offset & 0xFF);
            pack1024[9]  = (unsigned char)((offset >> 8) & 0xFF);
            pack1024[10] = (unsigned char)((offset >> 16) & 0xFF);
            pack1024[11] = (unsigned char)((offset >> 24) & 0xFF);

            memcpy(&pack1024[12], &rgb565[offset], chunk);
            
            int len_data = chunk + 4; 
            int total_len_data = len_data + 4; 
            
            pack1024[0] = 0x22; 
            pack1024[1] = 0x04; 
            pack1024[4] = total_len_data & 0xFF; 
            pack1024[5] = (total_len_data >> 8) & 0xFF; 
            pack1024[6] = 0x25; 

            unsigned int hash_data = 0;
            int target_len_data = total_len_data + 4;
            if (target_len_data % 2 != 0) target_len_data++;
            
            for (int i = 0; i < target_len_data; i += 2) {
                unsigned short word = (unsigned short)pack1024[i] | ((unsigned short)pack1024[i + 1] << 8);
                hash_data += word;
            }
            pack1024[2] = hash_data & 0xFF; 
            pack1024[3] = (hash_data >> 8) & 0xFF; 
            
            libusb_bulk_transfer(global_dev_handle, EP_OUT, pack1024, 1024, &actual_len, 1000);
        }
    }

    return 0;
}
