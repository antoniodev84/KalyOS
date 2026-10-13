#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
static int failure(const char *name) {
    perror(name);
    return 1;
}
static int configure(int fd, int rate, int channels) {
    int format = AFMT_S16_LE;
    return ioctl(fd, SNDCTL_DSP_SETFMT, &format) || ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) ||
           ioctl(fd, SNDCTL_DSP_SPEED, &rate);
}
static int output(int fd, const void *buffer, size_t length) {
    const uint8_t *data = buffer;
    while (length) {
        ssize_t count = write(fd, data, length);
        if (count <= 0) {
            return -1;
        }
        data += count;
        length -= count;
    }
    return 0;
}
static int tone(int fd, unsigned frequency, unsigned milliseconds) {
    if (!frequency || frequency > 10000 || !milliseconds || milliseconds > 60000) {
        return 2;
    }
    if (configure(fd, 48000, 2)) {
        return failure("audio format");
    }
    uint64_t remaining = milliseconds * 48UL;
    unsigned phase = 0;
    while (remaining) {
        int16_t buffer[2048];
        unsigned count = remaining > 1024 ? 1024 : remaining;
        for (unsigned i = 0; i < count; i++) {
            phase = (phase + frequency) % 48000;
            int sample = phase < 24000 ? (int)phase * 16000 / 24000 - 8000
                                       : 24000 - (int)phase * 16000 / 24000;
            buffer[2 * i] = buffer[2 * i + 1] = sample;
        }
        if (output(fd, buffer, count * 4)) {
            return failure("audio write");
        }
        remaining -= count;
    }
    return ioctl(fd, SNDCTL_DSP_SYNC, NULL) ? failure("audio sync") : 0;
}
static int read_exact(int fd, void *data, size_t length) {
    uint8_t *p = data;
    while (length) {
        ssize_t count = read(fd, p, length);
        if (count <= 0) {
            return -1;
        }
        p += count;
        length -= count;
    }
    return 0;
}
static uint16_t little16(const uint8_t *p) {
    return p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t little32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int play(int audio_fd, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return failure(path);
    }
    uint8_t header[12];
    int result = 2;
    if (read_exact(fd, header, 12) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        goto end;
    }
    uint64_t remaining = little32(header + 4);
    if (remaining < 4) {
        goto end;
    }
    remaining -= 4;
    bool configured = false;
    unsigned frame_size = 0;
    while (remaining >= 8) {
        uint8_t chunk[8];
        if (read_exact(fd, chunk, 8)) {
            goto end;
        }
        remaining -= 8;
        uint32_t length = little32(chunk + 4);
        if (length > remaining || (uint64_t)length + (length & 1) > remaining) {
            goto end;
        }
        if (!memcmp(chunk, "fmt ", 4)) {
            uint8_t format[16];
            if (length < 16 || read_exact(fd, format, 16) || little16(format) != 1 ||
                little16(format + 14) != 16) {
                goto end;
            }
            unsigned channels = little16(format + 2), rate = little32(format + 4);
            if ((channels != 1 && channels != 2) || (rate != 44100 && rate != 48000) ||
                little16(format + 12) != channels * 2 ||
                little32(format + 8) != rate * channels * 2) {
                goto end;
            }
            if (configure(audio_fd, rate, channels)) {
                result = failure("audio format");
                goto end;
            }
            frame_size = channels * 2;
            configured = true;
            if (lseek(fd, length - 16, SEEK_CUR) < 0) {
                goto end;
            }
        } else if (!memcmp(chunk, "data", 4)) {
            if (!configured || length % frame_size) {
                goto end;
            }
            uint8_t buffer[4096];
            unsigned bytes = length;
            while (bytes) {
                unsigned count = bytes > sizeof(buffer) ? sizeof(buffer) : bytes;
                if (read_exact(fd, buffer, count)) {
                    goto end;
                }
                if (output(audio_fd, buffer, count)) {
                    result = failure("audio write");
                    goto end;
                }
                bytes -= count;
            }
            result = ioctl(audio_fd, SNDCTL_DSP_SYNC, NULL) ? failure("audio sync") : 0;
            goto end;
        } else if (lseek(fd, length, SEEK_CUR) < 0) {
            goto end;
        }
        if ((length & 1) && lseek(fd, 1, SEEK_CUR) < 0) {
            goto end;
        }
        remaining -= length + (length & 1);
    }
end:
    if (result == 2) {
        fputs("sound: WAV PCM s16le mono/stereo 44100/48000 required\n", stderr);
    }
    close(fd);
    return result;
}
int sound_command(int argc, char **argv) {
    if (argc < 2) {
        puts("sound tone [Hz] [milliseconds] | play file.wav | volume 0..100");
        return 2;
    }
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        return failure("/dev/dsp");
    }
    int result = 2;
    if (!strcmp(argv[1], "tone") && argc <= 4) {
        result = tone(fd, argc > 2 ? strtoul(argv[2], NULL, 10) : 440,
                      argc > 3 ? strtoul(argv[3], NULL, 10) : 1000);
    } else if (!strcmp(argv[1], "play") && argc == 3) {
        result = play(fd, argv[2]);
    } else if (!strcmp(argv[1], "volume") && argc == 3) {
        unsigned volume = strtoul(argv[2], NULL, 10);
        if (volume <= 100) {
            int value = volume | (volume << 8);
            result = ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &value) ? failure("volume") : 0;
        }
    }
    close(fd);
    return result;
}
