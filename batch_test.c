#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "midiplayer.h"

#define NOTE_HEIGHT 6
#define MAX_KEYS 128
#define FLASH_DURATION 0.15f

#define RING_BUFFER_SIZE 8192
#define MAX_RENDERED_NOTES 200000
#define BATCH_SIZE 10000  // For batched rendering

typedef struct {
    uint8_t note;
    uint8_t velocity;
    bool isNoteOn;
    double timestamp;
} MidiEvent;

typedef struct {
    MidiEvent events[RING_BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
} EventQueue;

typedef struct {
    uint8_t note;
    double startTime;
    double endTime;
    uint8_t velocity;
    bool active;
    float flashTimer;
} NoteEvent;

typedef struct {
    NoteEvent* notes;
    int capacity;
    int count;
} NoteArray;

static EventQueue eventQueue = {0};
static NoteArray activeNotes = {0};
static double globalTime = 0.0;
static double timeOffset = 0.0;
float scrollSpeed = 400.0f;
static RenderTexture2D pianoRollTexture;
static bool textureNeedsUpdate = true;
static int screenWidth = 1600;
static int screenHeight = 900;
static double lastRenderTime = 0.0;
static double renderInterval = 1.0 / 60.0;  // Reduced texture update rate
static Rectangle* noteRects = NULL;  // For batched rendering
static Color* noteColors = NULL;     // For batched rendering
static int batchCount = 0;

static void init_event_queue() {
    eventQueue.head = 0;
    eventQueue.tail = 0;
    pthread_mutex_init(&eventQueue.mutex, NULL);
}

static bool queue_push(MidiEvent event) {
    bool success = false;
    pthread_mutex_lock(&eventQueue.mutex);
    int next = (eventQueue.head + 1) % RING_BUFFER_SIZE;
    if (next != eventQueue.tail) {
        eventQueue.events[eventQueue.head] = event;
        eventQueue.head = next;
        success = true;
    }
    pthread_mutex_unlock(&eventQueue.mutex);
    return success;
}

static bool queue_pop(MidiEvent* event) {
    bool success = false;
    pthread_mutex_lock(&eventQueue.mutex);
    if (eventQueue.tail != eventQueue.head) {
        *event = eventQueue.events[eventQueue.tail];
        eventQueue.tail = (eventQueue.tail + 1) % RING_BUFFER_SIZE;
        success = true;
    }
    pthread_mutex_unlock(&eventQueue.mutex);
    return success;
}

inline __attribute__((always_inline)) static int findFirstVisible(double visibleStartTime) {
    // Binary search for the first note with startTime >= visibleStartTime.
    int lo = 0, hi = activeNotes.count - 1, mid, index = activeNotes.count;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        if (activeNotes.notes[mid].startTime < visibleStartTime) {
            lo = mid + 1;
        } else {
            index = mid;
            hi = mid - 1;
        }
    }
    return index;
}

static void init_note_array() {
    activeNotes.capacity = 1024;
    activeNotes.count = 0;
    activeNotes.notes = (NoteEvent*)malloc(sizeof(NoteEvent) * activeNotes.capacity);

    // Initialize batch rendering buffers
    noteRects = (Rectangle*)malloc(sizeof(Rectangle) * BATCH_SIZE);
    noteColors = (Color*)malloc(sizeof(Color) * BATCH_SIZE);
}

inline __attribute__((always_inline)) static void ensure_note_capacity() {
    if (activeNotes.count >= activeNotes.capacity) {
        activeNotes.capacity *= 2;
        activeNotes.notes = (NoteEvent*)realloc(activeNotes.notes, sizeof(NoteEvent) * activeNotes.capacity);
    }
}

inline __attribute__((always_inline)) static float get_note_y_piano(uint8_t note) {
    return screenHeight - ((float)(note + 1) / MAX_KEYS) * screenHeight;
}

inline __attribute__((always_inline)) static float get_note_y(uint8_t note) {
    return ((float)(note + 1) / MAX_KEYS) * screenHeight;
}

inline __attribute__((always_inline)) static void note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    MidiEvent event = {
        .note = note,
        .velocity = velocity,
        .isNoteOn = true,
        .timestamp = GetTime() - timeOffset
    };
    queue_push(event);
}

inline __attribute__((always_inline)) static void note_off(uint8_t channel, uint8_t note) {
    MidiEvent event = {
        .note = note,
        .velocity = 0,
        .isNoteOn = false,
        .timestamp = GetTime() - timeOffset
    };
    queue_push(event);
}

inline __attribute__((always_inline)) static void process_midi_events() {
    MidiEvent event;
    bool changed = false;

    while (queue_pop(&event)) {
        if (event.isNoteOn) {
            ensure_note_capacity();
            NoteEvent newNote = {
                .note = event.note,
                .startTime = event.timestamp,
                .endTime = -1.0,
                .velocity = event.velocity,
                .active = true,
                .flashTimer = FLASH_DURATION
            };
            activeNotes.notes[activeNotes.count++] = newNote;
            changed = true;
        } else {
            for (int i = activeNotes.count - 1; i >= 0; i--) {
                if (activeNotes.notes[i].note == event.note && activeNotes.notes[i].active && activeNotes.notes[i].endTime < 0.0) {
                    activeNotes.notes[i].endTime = event.timestamp;
                    activeNotes.notes[i].active = false;
                    changed = true;
                    break;
                }
            }
        }
    }

    if (changed) {
        textureNeedsUpdate = true;
    }
}

inline __attribute__((always_inline)) static void cleanup_notes() {
    // static double lastCleanupTime = 0.0;
    //
    // // Only run cleanup periodically
    // if (globalTime - lastCleanupTime < 1.0) {
    //     return;
    // }

    // lastCleanupTime = globalTime;
    double cutoffTime = globalTime - (screenWidth / scrollSpeed);
    int writeIndex = 0;
    bool changed = false;

    // First pass: remove notes that ended too early
    for (int i = 0; i < activeNotes.count; i++) {
        bool isOld = activeNotes.notes[i].endTime >= 0 && activeNotes.notes[i].endTime < cutoffTime;
        if (!isOld) {
            if (writeIndex != i) {
                activeNotes.notes[writeIndex] = activeNotes.notes[i];
            }
            writeIndex++;
        } else {
            changed = true;
        }
    }

    int originalCount = activeNotes.count;
    activeNotes.count = writeIndex;

    // Second pass: trim oldest notes if we exceed the max count
    if (activeNotes.count > MAX_RENDERED_NOTES) {
        int extra = activeNotes.count - MAX_RENDERED_NOTES;
        memmove(activeNotes.notes, activeNotes.notes + extra, sizeof(NoteEvent) * (activeNotes.count - extra));
        activeNotes.count -= extra;
        changed = true;
    }

    if (changed || originalCount != activeNotes.count) {
        textureNeedsUpdate = true;
    }
}

inline __attribute__((always_inline)) static void update_texture() {
    double currentTime = GetTime() - timeOffset;

    // Skip rendering if not enough time has passed and no changes
    if (!textureNeedsUpdate && (currentTime - lastRenderTime < renderInterval)) {
        return;
    }

    lastRenderTime = currentTime;
    BeginTextureMode(pianoRollTexture);
    ClearBackground(BLACK);

    // Draw octave markers
    for (int octave = 0; octave < 11; octave++) {
        int baseNote = octave * 12;
        Color lineColor = (Color){ 30, 30, 30, 255 };
        if (baseNote < MAX_KEYS) {
            float y = get_note_y(baseNote);
            DrawLine(0, y, screenWidth, y, lineColor);
        }
    }

    // Calculate visible range
    double visibleStartTime = globalTime - screenWidth / scrollSpeed;
    double visibleEndTime = globalTime;

    // Batched rendering of notes
    batchCount = 0;

    int firstVisible = findFirstVisible(visibleStartTime);
    for (int i = firstVisible; i < activeNotes.count; i++) {
        NoteEvent* ev = &activeNotes.notes[i];
        // If ev->startTime > visibleEndTime, break early.
        if (ev->startTime > visibleEndTime) break;

        // Early culling - skip notes completely outside visible area
        double noteEndTime = (ev->endTime >= 0.0) ? ev->endTime : globalTime;
        if (noteEndTime < visibleStartTime || ev->startTime > visibleEndTime) {
            continue;
        }

        float duration = noteEndTime - ev->startTime;
        float x = (ev->startTime - visibleStartTime) * scrollSpeed;
        float width = duration * scrollSpeed;
        float y = get_note_y(ev->note);

        float intensity = ev->velocity / 127.0f;
        Color color = (Color){ (int)(intensity * 255), 64, 255, 255 };

        // Add to batch buffer
        noteRects[batchCount] = (Rectangle){ x, y - NOTE_HEIGHT, width, NOTE_HEIGHT };
        noteColors[batchCount] = color;
        batchCount++;

        // Draw batch if buffer is full
        if (batchCount == BATCH_SIZE) {
            for (int j = 0; j < batchCount; j++) {
                DrawRectangleRec(noteRects[j], noteColors[j]);
            }
            batchCount = 0;
        }
    }

    // Draw remaining notes in batch
    for (int j = 0; j < batchCount; j++) {
        DrawRectangleRec(noteRects[j], noteColors[j]);
    }

    EndTextureMode();
    textureNeedsUpdate = false;
}

static void* midi_thread(void* arg) {
    char* midiPath = (char*)arg;
    timeOffset = GetTime();
    PlayMIDI(midiPath, note_on, note_off);
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <midi_file>\n", argv[0]);
        return 1;
    }

    init_event_queue();
    init_note_array();

    InitWindow(screenWidth, screenHeight, "Optimized MIDI Piano Roll");
    SetTargetFPS(144);
    pianoRollTexture = LoadRenderTexture(screenWidth, screenHeight);

    pthread_t midiThread;
    pthread_create(&midiThread, NULL, midi_thread, argv[1]);

    while (!WindowShouldClose()) {
        double currentTime = GetTime() - timeOffset;
        double deltaTime = currentTime - globalTime;
        globalTime = currentTime;

        // Process new MIDI events
        process_midi_events();

        // Clean up old notes periodically
        cleanup_notes();

        // Update texture only when needed
        update_texture();

        // Update flash timers
        for (int i = 0; i < activeNotes.count; i++) {
            if (activeNotes.notes[i].flashTimer > 0.0f) {
                activeNotes.notes[i].flashTimer -= deltaTime;
            }
        }

        BeginDrawing();
        ClearBackground(BLACK);

        // Draw the piano roll texture
        DrawTexture(pianoRollTexture.texture, 0, 0, WHITE);

        // Draw piano keyboard
        const int keyboardWidth = 40;
        DrawRectangle(screenWidth - keyboardWidth, 0, keyboardWidth, screenHeight, DARKGRAY);

        // Draw black keys and octave labels
        for (int note = 0; note < MAX_KEYS; note++) {
            int noteType = note % 12;
            float y = get_note_y(note);
            bool isBlackKey = (noteType == 1 || noteType == 3 || noteType == 6 || noteType == 8 || noteType == 10);
            if (isBlackKey) {
                DrawRectangle(screenWidth - keyboardWidth/2, y - NOTE_HEIGHT, keyboardWidth/2, NOTE_HEIGHT, BLACK);
            }
            if (noteType == 0) {
                DrawText(TextFormat("C%d", note/12 - 1), screenWidth - keyboardWidth + 2, y - NOTE_HEIGHT - 8, 10, GRAY);
            }
        }

        // Draw currently pressed keys
        batchCount = 0;
        for (int i = 0; i < activeNotes.count; i++) {
            NoteEvent* ev = &activeNotes.notes[i];
            if (ev->flashTimer > 0.0f) {
                float y = get_note_y_piano(ev->note);
                float alpha = ev->flashTimer / FLASH_DURATION;
                Color flashColor = WHITE;
                flashColor.a = (unsigned char)(alpha * 255);

                noteRects[batchCount] = (Rectangle){ screenWidth - keyboardWidth, y - NOTE_HEIGHT, keyboardWidth, NOTE_HEIGHT };
                noteColors[batchCount] = flashColor;
                batchCount++;

                if (batchCount == BATCH_SIZE) {
                    for (int j = 0; j < batchCount; j++) {
                        DrawRectangleRec(noteRects[j], noteColors[j]);
                    }
                    batchCount = 0;
                }
            }
        }

        // Draw remaining keys in batch
        for (int j = 0; j < batchCount; j++) {
            DrawRectangleRec(noteRects[j], noteColors[j]);
        }

        DrawLine(screenWidth - keyboardWidth, 0, screenWidth - keyboardWidth, screenHeight, WHITE);
        DrawFPS(10, 10);
        DrawText(TextFormat("Notes: %d", activeNotes.count), 10, 30, 20, GREEN);

        EndDrawing();
    }

    UnloadRenderTexture(pianoRollTexture);
    free(activeNotes.notes);
    free(noteRects);
    free(noteColors);
    CloseWindow();
    return 0;
}