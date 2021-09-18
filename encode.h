#ifndef ENCODE_H
#define ENCODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>

typedef struct {
    void *priv;
} encoder_t;


int encoder_setup(encoder_t *encoder, const char *cammand, int ofd);

int encoder_frame(encoder_t *encoder, void *data, size_t len, int64_t pts);

int encoder_release(encoder_t *encoder);


#ifdef __cplusplus
}
#endif


#endif // ENCODE_H
