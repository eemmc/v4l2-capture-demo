#ifndef V4L2_H
#define V4L2_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

typedef struct {
    void *data;
    size_t len;

    void *priv;
} v4l2_capture_t;


int v4l2_capture_setup(v4l2_capture_t *ptr, int fd);

int v4l2_capture_wait(v4l2_capture_t *ptr);

int v4l2_capture_notify(v4l2_capture_t *ptr);

int v4l2_capture_release(v4l2_capture_t *ptr);


#ifdef __cplusplus
}
#endif

#endif // V4L2_H
