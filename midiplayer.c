#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>

inline __attribute__((always_inline)) uint32_t fntohl(uint32_t nlong) {
    return ((nlong & 0xFF000000) >> 24) |
    ((nlong & 0x00FF0000) >> 8)  |
    ((nlong & 0x0000FF00) << 8)  |
    ((nlong & 0x000000FF) << 24);
}

inline __attribute__((always_inline)) uint16_t fntohs(uint16_t nshort) {
    return ((nshort & 0xFF00) >> 8) |
    ((nshort & 0x00FF) << 8);
}

// Track data structure
typedef struct {
    uint8_t* data;
    uint8_t* long_msg;
    int tick;
    size_t offset, length;
    uint32_t message, temp;
    size_t long_msg_len;
    size_t long_msg_capacity;
    size_t data_capacity;
} TrackData;

// Function pointer type for SendDirectData
typedef void (*SendDirectDataFunc)(uint32_t);

// Function declarations
int decode_variable_length(TrackData* track);
void update_tick(TrackData* track);
void update_command(TrackData* track);
void update_message(TrackData* track);
void process_meta_event(TrackData* track, double* multiplier, uint64_t* bpm, uint16_t time_div);
void* log_notes_per_second(void* arg);

uint64_t get100NanosecondsSinceEpoch() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * 10000000ULL) +
    ((uint64_t)ts.tv_nsec / 100ULL);
}

void delayExecution100Ns(int64_t delayIn100Ns) {
    struct timespec req = {0};
    req.tv_sec = delayIn100Ns / 10000000;
    req.tv_nsec = (delayIn100Ns % 10000000) * 100;
    nanosleep(&req, NULL);
}

// Implementation of track functions
inline __attribute__((always_inline)) int decode_variable_length(TrackData* track) {
    int result = 0;
    uint8_t byte;
    if (track->offset >= track->length) return 0; // Avoid out-of-bounds
    do {
        byte = track->data[track->offset++];
        result = (result << 7) | (byte & 0x7F);
    } while ((byte & 0x80) && track->offset < track->length); // Prevent overflow
    return result;
}

inline __attribute__((always_inline)) void update_tick(TrackData* track) {
    track->tick += decode_variable_length(track);
}

inline __attribute__((always_inline)) void update_command(TrackData* track) {
    if (track->length == 0 || track->data == NULL)
        return;
    const uint8_t temp = track->data[track->offset];
    if (temp >= 0x80) {
        track->offset++;
        track->message = temp;
    }
}

inline __attribute__((always_inline)) void update_message(TrackData* track) {
    if (track->length == 0 || track->data == NULL) return;

    const uint8_t msg_type = track->message & 0xFF;

    if (msg_type < 0xC0) {
        track->temp = track->data[track->offset] << 8 | track->data[track->offset + 1] << 16;
        track->offset += 2;
    } else if (msg_type < 0xE0) {
        track->temp = track->data[track->offset] << 8;
        track->offset += 1;
    } else if (msg_type < 0xF0) {
        track->temp = track->data[track->offset] << 8 | track->data[track->offset + 1] << 16;
        track->offset += 2;
    } else if (msg_type == 0xFF || msg_type == 0xF0) {
        track->temp = (msg_type == 0xFF) ? track->data[track->offset] << 8 : 0;
        track->offset += 1;
        track->long_msg_len = decode_variable_length(track);

        // Ensure we have enough capacity
        if (track->long_msg_capacity < track->long_msg_len) {
            printf("Alloc");
            uint8_t* new_buf = realloc(track->long_msg, track->long_msg_len);
            if (new_buf == NULL) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(1);
            }
            track->long_msg = new_buf;
            track->long_msg_capacity = track->long_msg_len;
        }

        memcpy(track->long_msg, &track->data[track->offset], track->long_msg_len);
        track->offset += track->long_msg_len;
    }

    track->message |= track->temp;
}

inline __attribute__((always_inline)) void process_meta_event(TrackData* track, double* multiplier, uint64_t* bpm, uint16_t time_div) {
    const uint8_t meta_type = (track->message >> 8) & 0xFF;
    if (meta_type == 0x51) { // Tempo change
        *bpm = (track->long_msg[0] << 16) | (track->long_msg[1] << 8) | track->long_msg[2];
        *multiplier = (double)(*bpm * 10) / (double)time_div;
        *multiplier = (*multiplier < 1.0) ? 1.0 : *multiplier; // Ensure minimum multiplier of 1
    }
    else if (meta_type == 0x2F) { // End of track
        free(track->data);
        track->data = NULL;
        track->length = 0;
    }
}

typedef struct {
    bool* is_playing;
    uint64_t* note_on_count;
} LoggerArgs;

void* log_notes_per_second(void* arg) {
    LoggerArgs* args = (LoggerArgs*)arg;
    bool* is_playing = args->is_playing;
    uint64_t* note_on_count = args->note_on_count;

    while (*is_playing) {
        struct timespec sleep_time = {1, 0}; // 1 second
        nanosleep(&sleep_time, NULL);
        printf("Notes per second: %lu\n", *note_on_count);
        *note_on_count = 0;
    }

    free(args);
    return NULL;
}

void init_track_data(TrackData* track) {
    track->data = NULL;
    track->long_msg = NULL;
    track->tick = 0;
    track->offset = 0;
    track->length = 0;
    track->message = 0;
    track->temp = 0;
    track->long_msg_len = 0;
    track->long_msg_capacity = 0;
    track->data_capacity = 0;
}

void free_track_data(TrackData* track) {
    if (track->data) free(track->data);
    if (track->long_msg) free(track->long_msg);
    track->data = NULL;
    track->long_msg = NULL;
}

typedef void (*NoteOnCallback)(uint8_t velocity, uint8_t channel, uint8_t note);
typedef void (*NoteOffCallback)(uint8_t channel, uint8_t note);

void play_midi(TrackData* tracks, int track_count, uint16_t time_div,
               NoteOnCallback note_on_callback, NoteOffCallback note_off_callback) {
    uint64_t tick = 0;
    double multiplier = 0;
    uint64_t bpm = 500000; // Default tempo: 120 BPM
    uint64_t delta_tick = 0;
    uint64_t last_time = 0;
    const uint64_t max_drift = 100000;
    int64_t delta = 0;
    uint64_t old = 0;
    uint64_t temp = 0;

    uint64_t note_on_count = 0;
    bool is_playing = true;

    uint64_t now = get100NanosecondsSinceEpoch();
    last_time = now;

    // Setup and start logger thread
    pthread_t logger_thread;
    LoggerArgs* logger_args = malloc(sizeof(LoggerArgs));
    logger_args->is_playing = &is_playing;
    logger_args->note_on_count = &note_on_count;

    pthread_create(&logger_thread, NULL, log_notes_per_second, logger_args);

    bool has_active_tracks = true;

    while (has_active_tracks) {
        // Check if there are any active tracks
        has_active_tracks = false;
        int active_track_count = 0;
        int* active_tracks = malloc(track_count * sizeof(int));

        for (int i = 0; i < track_count; i++) {
            if (tracks[i].data != NULL) {
                if (tracks[i].tick <= tick) {
                    // Process events in this track
                    while (tracks[i].data != NULL && tracks[i].tick <= tick) {
                        update_command(&tracks[i]);
                        update_message(&tracks[i]);

                        const uint32_t message = tracks[i].message;
                        const uint8_t status_byte = message & 0xFF;
                        const uint8_t channel = status_byte & 0x0F;
                        const uint8_t command = status_byte & 0xF0;
                        const uint8_t note = (message >> 8) & 0xFF;
                        const uint8_t velocity = (message >> 16) & 0xFF;

                        if (command == 0x90) { // Note On
                            note_on_count++;
                            if (velocity > 0) {
                                if (note_on_callback) {
                                    note_on_callback(velocity, channel, note);
                                }
                            } else {
                                if (note_off_callback) {
                                    note_off_callback(channel, note);
                                }
                            }
                        } else if (command == 0x80) { // Note Off
                            if (note_off_callback) {
                                note_off_callback(channel, note);
                            }
                        } else if (status_byte == 0xFF) {
                            process_meta_event(&tracks[i], &multiplier, &bpm, time_div);
                        } else if (status_byte == 0xF0) {
                            printf("TODO: Handle SysEx\n");
                        }

                        if (tracks[i].data != NULL) {
                            update_tick(&tracks[i]);
                        }
                    }
                }

                if (tracks[i].data != NULL) {
                    active_tracks[active_track_count++] = i;
                    has_active_tracks = true;
                }
            }
        }

        if (!has_active_tracks) {
            free(active_tracks);
            break;
        }

        // Find next tick
        delta_tick = UINT64_MAX;
        for (int i = 0; i < active_track_count; i++) {
            int idx = active_tracks[i];
            if (tracks[idx].data != NULL) {
                uint64_t track_delta = tracks[idx].tick - tick;
                if (track_delta < delta_tick) {
                    delta_tick = track_delta;
                }
            }
        }

        free(active_tracks);

        tick += delta_tick;

        now = get100NanosecondsSinceEpoch();
        temp = now - last_time;
        last_time = now;
        temp -= old;
        old = delta_tick * multiplier;
        delta += temp;

        temp = (delta > 0) ? (old - delta) : old;

        if (temp <= 0) {
            delta = (delta < (int64_t)max_drift) ? delta : (int64_t)max_drift;
        } else {
            delayExecution100Ns(temp);
        }
    }

    is_playing = false;
    pthread_join(logger_thread, NULL);
}

TrackData* load_midi_file(const char* filename, uint16_t* time_div, int* track_count) {
    TrackData* tracks = NULL;
    FILE* file = fopen(filename, "rb");

    if (!file) {
        fprintf(stderr, "Could not open file\n");
        return NULL;
    }

    clock_t start_time = clock();

    char header[4];
    fread(header, 1, 4, file);
    if (strncmp(header, "MThd", 4) != 0) {
        fprintf(stderr, "Not a MIDI file\n");
        fclose(file);
        return NULL;
    }

    uint32_t header_length;
    fread(&header_length, 4, 1, file);
    header_length = fntohl(header_length);
    if (header_length != 6) {
        fprintf(stderr, "Invalid header length\n");
        fclose(file);
        return NULL;
    }

    uint16_t format;
    fread(&format, 2, 1, file);
    format = fntohs(format);

    uint16_t num_tracks;
    fread(&num_tracks, 2, 1, file);
    num_tracks = fntohs(num_tracks);

    fread(time_div, 2, 1, file);
    *time_div = fntohs(*time_div);

    if (*time_div >= 0x8000) {
        fprintf(stderr, "SMPTE timing not supported\n");
        fclose(file);
        return NULL;
    }

    printf("%d tracks\n", num_tracks);

    tracks = malloc(num_tracks * sizeof(TrackData));
    if (!tracks) {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    // Initialize tracks
    for (int i = 0; i < num_tracks; i++) {
        init_track_data(&tracks[i]);
    }

    int valid_tracks = 0;
    for (int i = 0; i < num_tracks; ++i) {
        fread(header, 1, 4, file);
        if (strncmp(header, "MTrk", 4) != 0) {
            continue;
        }

        uint32_t length;
        fread(&length, 4, 1, file);
        length = fntohl(length);

        // Allocate memory for track data
        tracks[valid_tracks].data = malloc(length);
        if (!tracks[valid_tracks].data) {
            fprintf(stderr, "Memory allocation failed\n");
            for (int j = 0; j < valid_tracks; j++) {
                free_track_data(&tracks[j]);
            }
            free(tracks);
            fclose(file);
            return NULL;
        }

        // Allocate initial long message buffer
        tracks[valid_tracks].long_msg = malloc(256);
        if (!tracks[valid_tracks].long_msg) {
            fprintf(stderr, "Memory allocation failed\n");
            free(tracks[valid_tracks].data);
            for (int j = 0; j < valid_tracks; j++) {
                free_track_data(&tracks[j]);
            }
            free(tracks);
            fclose(file);
            return NULL;
        }

        tracks[valid_tracks].long_msg_capacity = 256;
        tracks[valid_tracks].length = length;
        tracks[valid_tracks].data_capacity = length;
        tracks[valid_tracks].tick = 0;
        tracks[valid_tracks].offset = 0;
        tracks[valid_tracks].message = 0;
        tracks[valid_tracks].temp = 0;

        fread(tracks[valid_tracks].data, 1, length, file);
        update_tick(&tracks[valid_tracks]);
        valid_tracks++;
    }

    *track_count = valid_tracks;

    const clock_t end_time = clock();
    const double duration_seconds = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    const long duration_microseconds = (long)(duration_seconds * 1000000);
    const long duration_milliseconds = (long)(duration_seconds * 1000);

    printf("Parsed in %ldms (%ldμs).\n", duration_milliseconds, duration_microseconds);

    fclose(file);
    return tracks;
}

void* initialize_midi(SendDirectDataFunc* SendDirectData) {
    void* midi_lib = dlopen("./libOmniMIDI.so", RTLD_LAZY);

    if (!midi_lib) {
        fprintf(stderr, "Failed to load OmniMIDI.so: %s\n!!!", dlerror());
        return NULL;
    }

    // Clear any existing error
    dlerror();

    bool (*IsKDMAPIAvailable)() = dlsym(midi_lib, "IsKDMAPIAvailable");
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        fprintf(stderr, "Cannot load IsKDMAPIAvailable: %s\n", dlsym_error);
        dlclose(midi_lib);
        return NULL;
    }

    bool (*InitializeKDMAPIStream)() = dlsym(midi_lib, "InitializeKDMAPIStream");
    dlsym_error = dlerror();
    if (dlsym_error) {
        fprintf(stderr, "Cannot load InitializeKDMAPIStream: %s\n", dlsym_error);
        dlclose(midi_lib);
        return NULL;
    }

    if (!IsKDMAPIAvailable || !InitializeKDMAPIStream ||
        !IsKDMAPIAvailable() || !InitializeKDMAPIStream()) {
        fprintf(stderr, "MIDI initialization failed\n");
    dlclose(midi_lib);
    return NULL;
        }

        *SendDirectData = dlsym(midi_lib, "SendDirectData");
        dlsym_error = dlerror();
        if (dlsym_error) {
            fprintf(stderr, "Cannot load SendDirectData: %s\n", dlsym_error);
            dlclose(midi_lib);
            return NULL;
        }

        return midi_lib;
}

// int main(const int argc, char* argv[]) {
//     if (argc < 2) {
//         fprintf(stderr, "Usage: %s <midi_file>\n", argv[0]);
//         return 1;
//     }
//
//     // Initialize MIDI
//     SendDirectDataFunc SendDirectData;
//     const clock_t start_time = clock();
//
//     void* midi_lib = initialize_midi(&SendDirectData);
//     if (!midi_lib) {
//         return 1;
//     }
//
//     // Load MIDI file
//     uint16_t time_div = 0;
//     int track_count = 0;
//     TrackData* tracks = load_midi_file(argv[1], &time_div, &track_count);
//     if (!tracks) {
//         dlclose(midi_lib);
//         return 1;
//     }
//
//     const clock_t end_time = clock();
//     const double duration_seconds = (double)(end_time - start_time) / CLOCKS_PER_SEC;
//     const long duration_milliseconds = (long)(duration_seconds * 1000);
//
//     printf("MIDI initialization took %ldms.\n", duration_milliseconds);
//     printf("\n\n\nPlaying midi file: %s\n", argv[1]);
//
//     play_midi(tracks, track_count, time_div, SendDirectData);
//
//     // Clean up
//     for (int i = 0; i < track_count; i++) {
//         free_track_data(&tracks[i]);
//     }
//     free(tracks);
//     dlclose(midi_lib);
//
//     return 0;
// }
