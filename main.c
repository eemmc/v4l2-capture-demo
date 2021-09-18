#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v4l2.h"
#include "encode.h"

static const char DEVICE[] = "/dev/video0";
static const char OUTPUT[] = "test.h264";
static const char FILTER[] = "null";

int main(int argc, char **argv) {

    int vfd, ofd;
    encoder_t encoder;
    v4l2_capture_t capture;

    if((vfd = open(DEVICE, O_RDWR)) < 0){
        perror("DEVICE");
        return errno;
    }

    if((ofd = open(OUTPUT, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) < 0){
        fprintf(stderr, "Could not open %s\n", OUTPUT);
        exit(EXIT_FAILURE);
    }


    v4l2_capture_setup(&capture, vfd);
    encoder_setup(&encoder, FILTER, ofd);

    int64_t index = 0;
    // 录制100帧约10秒(10帧/秒)
    while(index < 100) {
        v4l2_capture_wait(&capture);

        encoder_frame(&encoder, capture.data, capture.len, index);

        v4l2_capture_notify(&capture);
        index++;
    }

    encoder_release(&encoder);
    v4l2_capture_release(&capture);

    close(ofd);
    close(vfd);
    printf("\n\nDone.\n");
    return 0;
}
