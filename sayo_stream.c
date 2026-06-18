#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
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

void* video_receiver_thread(void* arg) {
    unsigned char *local_buffer = malloc(rgb_size);
    while (1) {
        int bytes_read = 0;
        while (bytes_read < rgb_size) {
            int r = read(pipe_fd, local_buffer + bytes_read, rgb_size - bytes_read);
            if (r <= 0) {
                //usleep(1000);
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

int main() {
    libusb_context *ctx = NULL;
    libusb_device_handle *dev_handle = NULL;
    int actual_len;

    if (libusb_init(&ctx) < 0) return 1;
    dev_handle = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!dev_handle) {
        printf("[Error] SayoDevice not found.\n");
        libusb_exit(ctx);
        return 1;
    }

    if (libusb_kernel_driver_active(dev_handle, 1) == 1) {
        libusb_detach_kernel_driver(dev_handle, 1);
    }
    libusb_claim_interface(dev_handle, 1);

    // using /dev/video50
    FILE *ffmpeg_pipe = popen("ffmpeg -fflags nobuffer -flags low_delay -f v4l2 -i /dev/video50 -vf \"scale=160:80\" -f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null", "r");
    if (!ffmpeg_pipe) {
        printf("[Error] Failed to start FFmpeg pipe!\n");
        return 1;
    }
    pipe_fd = fileno(ffmpeg_pipe);

    global_rgb_buffer = malloc(rgb_size);
    memset(global_rgb_buffer, 0, rgb_size);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, video_receiver_thread, NULL);
    pthread_detach(thread_id);

    unsigned char rgb565[DEV_WIDTH * DEV_HEIGHT * 2];
    unsigned char local_rgb[DEV_WIDTH * DEV_HEIGHT * 3];
    unsigned char pack1024[1024];

    printf("[OK] USB streaming has started.\n");

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
        
        libusb_bulk_transfer(dev_handle, EP_OUT, pack1024, 1024, &actual_len, 1000);
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
            int total_len_data = len_data + 4; // 1020 bite
            
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
            
            libusb_bulk_transfer(dev_handle, EP_OUT, pack1024, 1024, &actual_len, 1000);
            //usleep(450); 
        }

        //usleep(16000); 
    }

    free(global_rgb_buffer);
    pclose(ffmpeg_pipe);
    libusb_release_interface(dev_handle, 1);
    libusb_attach_kernel_driver(dev_handle, 1);
    libusb_close(dev_handle);
    libusb_exit(ctx);
    return 0;
}
