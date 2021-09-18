#include "v4l2.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    void *data;
    size_t len;
} buffer_t;

typedef struct {
    struct v4l2_requestbuffers request;
    size_t buffer_count;
    buffer_t *buffers;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;

    int finished;
    struct timeval times[4];

    int vfd;
    void *refs;
} capture_config_t;


static int xioctl(int fd, int request, void *arg) {
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while(-1 == ret && EINTR == errno);

    return ret;
}

static int v4l2_capture_device_init(capture_config_t *config) {
    assert(config);

    // init device
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = 1280;
    fmt.fmt.pix.height = 720;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field  = V4L2_FIELD_NONE;

    if(-1 == xioctl(config->vfd, VIDIOC_S_FMT, &fmt)) {
        perror("VIDIOC_S_FMT");
        return errno;
    }

    // init mmap
    struct v4l2_requestbuffers *request = &config->request;
    request->type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request->memory = V4L2_MEMORY_MMAP;
    request->count  = 4;

    if(-1 == xioctl(config->vfd, VIDIOC_REQBUFS, request)){
        perror("VIDIOC_REQBUFS");
        return errno;
    }

    if(request->count < 2) {
        fprintf(stderr, "Not enough buffer memory.\n");
        return EXIT_FAILURE;
    }

    config->buffers = calloc(request->count, sizeof(buffer_t));
    assert(config->buffers);
    config->buffer_count = request->count;

    unsigned int i;
    struct v4l2_buffer buffer;
    for(i = 0; i < request->count; i++) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type   = request->type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;

        if(-1 == xioctl(config->vfd, VIDIOC_QUERYBUF, &buffer)) {
            perror("VIDIOC_QUERYBUF");
            return errno;
        }

        config->buffers[i].len  = buffer.length;
        config->buffers[i].data = mmap(
            NULL,
            buffer.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            config->vfd,
            buffer.m.offset
        );

        if(MAP_FAILED == config->buffers[i].data) {
            perror("mmap");
            return errno;
        }
    }

    return 0;
}

static int v4l2_capture_start(capture_config_t *config) {
    assert(config);

    unsigned int i;
    enum v4l2_buf_type type;
    struct v4l2_buffer buffer;
    for(i = 0; i < config->buffer_count; i++) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;

        if(-1 == xioctl(config->vfd, VIDIOC_QBUF, &buffer)) {
            perror("VIDIOC_QBUF");
            return errno;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(-1 == xioctl(config->vfd, VIDIOC_STREAMON, &type)) {
        perror("VIDIOC_STREAMON");
        return errno;
    }

    return 0;
}

static int v4l2_capture_stop(capture_config_t *config) {
    assert(config);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VBI_CAPTURE;
    if(-1 == xioctl(config->vfd, VIDIOC_STREAMOFF, &type)) {
        perror("VIDIOC_STREAMOFF");
        return errno;
    }

    return 0;
}

static int v4l2_capture_frame(capture_config_t *config) {
    assert(config);

    // check time delay.
    gettimeofday(&config->times[0], NULL);
    timersub(&config->times[0], &config->times[1], &config->times[2]);
    int64_t stamp = config->times[2].tv_sec *1000000 + config->times[2].tv_usec;
    if(stamp < 100000){
        usleep(100000 - stamp);
    }

    gettimeofday(&config->times[1], NULL);

    // take picture.
    pthread_mutex_lock(&config->mutex);


    struct v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if(-1 == xioctl(config->vfd, VIDIOC_DQBUF, &buffer)) {
        switch(errno) {
        case EAGAIN:
            return 0;
        case EIO:
        default:
            perror("VIDIOC_DQBUF");
            return errno;
        }
    }

    assert(buffer.index < config->buffer_count);
    // swap buffer pointer.
    v4l2_capture_t *capture = (v4l2_capture_t *)config->refs;
    capture->data = config->buffers[buffer.index].data;
    capture->len  = config->buffers[buffer.index].len;

    if(-1 == xioctl(config->vfd, VIDIOC_QBUF, &buffer)){
        perror("VIDIOC_QBUF");
        return errno;
    }


    pthread_cond_signal(&config->cond);
    pthread_mutex_unlock(&config->mutex);

    return 0;
}

static void *v4l2_capture_loop(void *ptr){
    capture_config_t *config = ptr;
    assert(config);

    v4l2_capture_start(config);

    int ret;
    fd_set fds;
    struct timeval timeout;
    while(!config->finished) {
        FD_ZERO(&fds);
        FD_SET(config->vfd, &fds);

        timeout.tv_sec  = 1;
        timeout.tv_usec = 0;

        ret = select(config->vfd + 1, &fds, NULL, NULL, &timeout);
        if(-1 == ret) {
            if(EINTR == errno) {
                continue;
            }

            perror("select");
            break;
        }

        v4l2_capture_frame(config);
    }

    v4l2_capture_stop(config);

    return NULL;
}



int v4l2_capture_setup(v4l2_capture_t *ptr, int fd) {
    assert(ptr);

    capture_config_t *config = (capture_config_t *)malloc(sizeof(capture_config_t));
    memset(config, 0, sizeof(capture_config_t));
    config->refs = ptr;
    config->vfd  = fd;
    ptr->priv = config;

    v4l2_capture_device_init(config);

    pthread_mutex_init(&config->mutex, NULL);
    pthread_cond_init(&config->cond, NULL);
    pthread_create(&config->thread, NULL, v4l2_capture_loop, config);

    return 0;
}


int v4l2_capture_wait(v4l2_capture_t *ptr) {
    assert(ptr);
    capture_config_t *config = (capture_config_t*)ptr->priv;
    assert(config);

    pthread_mutex_lock(&config->mutex);
    pthread_cond_wait(&config->cond, &config->mutex);

    return 0;
}

int v4l2_capture_notify(v4l2_capture_t *ptr) {
    assert(ptr);
    capture_config_t *config = (capture_config_t*)ptr->priv;
    assert(config);

    pthread_mutex_unlock(&config->mutex);

    return 0;
}

int v4l2_capture_release(v4l2_capture_t *ptr) {
    assert(ptr);
    capture_config_t *config = (capture_config_t*)ptr->priv;
    if(config == NULL) {
        return 0;
    }

    config->finished = 1;
    pthread_join(config->thread, NULL);
    pthread_cond_destroy(&config->cond);
    pthread_mutex_destroy(&config->mutex);

    unsigned int i;
    for(i = 0; i < config->request.count; i++) {
        munmap(config->buffers[i].data, config->buffers[i].len);
    }

    free(config->buffers);
    free(config);

    ptr->priv = NULL;
    ptr->data = NULL;
    ptr->len  = 0;

    return 0;
}
