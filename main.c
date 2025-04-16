#include <math.h>
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
#define MAX_CHANNELS 16
#define FLASH_DURATION 0.15f

#define SCROLL_TEXTURE_WIDTH 6400  // Width of the scrolling texture buffer (in pixels)
#define RING_BUFFER_SIZE 13414000

#define CLEAR_WIDTH_MULTIPLIER 1.5f

// Key animation parameters
#define KEY_ANIMATION_DURATION 0.5f  // Duration of key animation in seconds
#define KEYBOARD_WIDTH 20

typedef struct {
    uint8_t note;
    uint8_t velocity;
    uint8_t channel;
    bool isNoteOn;
    double timestamp;
} MidiEvent;

typedef struct {
    bool isActive;
    uint8_t velocity;
    double startTime;
    float startX;   
    bool needsDrawing; 
    double keyPressTime;
    double keyReleaseTime;
    bool keyIsPressed;    
} ActiveNote;

typedef struct {
    MidiEvent events[RING_BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
} EventQueue;

static EventQueue eventQueue = {0};
static double globalTime = 0.0;
static double timeOffset = 0.0;
static float scrollSpeed = 500.0f; // pixels per second
static RenderTexture2D scrollTexture;
static bool textureNeedsUpdate = false;   // true when new events have arrived
static int screenWidth = 1600;
static int screenHeight = 900;
static uint64_t notesPerSecond = 0;
static float scrollOffset = 0.0f;
static double deltaTime = 0.0;
static double previousDeltaTime = 0.0; // For smoothing

// Track active notes per channel and note number
static ActiveNote activeNotes[MAX_CHANNELS][MAX_KEYS] = {0};

// We'll track the time up to which events have been drawn:
static double drawnTime = 0.0;
static double lastClearTime = 0.0;  // Track when we last cleared the texture

static void init_event_queue() {
    eventQueue.head = 0;
    eventQueue.tail = 0;
    pthread_mutex_init(&eventQueue.mutex, NULL);

    for (int c = 0; c < MAX_CHANNELS; c++) {
        for (int n = 0; n < MAX_KEYS; n++) {
            activeNotes[c][n].isActive = false;
            activeNotes[c][n].velocity = 0;
            activeNotes[c][n].startTime = 0.0;
            activeNotes[c][n].startX = 0.0f;
            activeNotes[c][n].needsDrawing = false;
            activeNotes[c][n].keyPressTime = 0.0;
            activeNotes[c][n].keyReleaseTime = 0.0;
            activeNotes[c][n].keyIsPressed = false;
        }
    }
}

inline __attribute__((always_inline)) static bool queue_push(const MidiEvent event) {
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

inline __attribute__((always_inline)) static bool queue_pop(MidiEvent* event) {
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

inline __attribute__((always_inline)) static float get_note_y_piano(const uint8_t note) {
    return (screenHeight - ((float)(note + 1) / MAX_KEYS) * screenHeight) + NOTE_HEIGHT + 1;
}

inline __attribute__((always_inline)) static float get_note_y(const uint8_t note) {
    return ((float)(note + 1) / MAX_KEYS) * screenHeight;
}

inline __attribute__((always_inline)) static Color get_note_color(uint8_t channel) {
    static const Color channelColors[MAX_CHANNELS] = {
        RED, ORANGE, GOLD, GREEN, DARKGREEN, SKYBLUE, BLUE, DARKBLUE,
        PURPLE, MAGENTA, MAROON, BROWN, PINK, DARKGRAY, RAYWHITE, WHITE
    };
    return channelColors[channel % MAX_CHANNELS];
}

// Get an animation alpha value based on press/release time
inline __attribute__((always_inline)) static float get_key_animation_alpha(double pressTime, double releaseTime, bool isPressed, double currentTime) {
    float alpha = 0.0f;

    if (isPressed) {
        // Key is currently pressed - full brightness
        alpha = 1.0f;
    } else if (releaseTime > 0.0) {
        // Key was recently released - fade out
        float timeSinceRelease = currentTime - releaseTime;
        if (timeSinceRelease < KEY_ANIMATION_DURATION) {
            alpha = 1.0f - (timeSinceRelease / KEY_ANIMATION_DURATION);
        }
    }

    return alpha;
}

inline __attribute__((always_inline)) static void note_on(uint8_t channel, const uint8_t note, const uint8_t velocity) {
    MidiEvent event = {
        .note = note,
        .velocity = velocity,
        .channel = channel,
        .isNoteOn = true,
        .timestamp = GetTime() - timeOffset
    };

    activeNotes[channel][note].isActive = true;
    activeNotes[channel][note].velocity = velocity;
    activeNotes[channel][note].startTime = event.timestamp;
    activeNotes[channel][note].needsDrawing = true;  // Mark that this note needs initial drawing
    activeNotes[channel][note].keyPressTime = event.timestamp;
    activeNotes[channel][note].keyIsPressed = true;

    queue_push(event);
    textureNeedsUpdate = true;
}

inline __attribute__((always_inline)) static void note_off(uint8_t channel, const uint8_t note) {
    MidiEvent event = {
        .note = note,
        .velocity = 0,
        .channel = channel,
        .isNoteOn = false,
        .timestamp = GetTime() - timeOffset
    };

    activeNotes[channel][note].isActive = false;
    activeNotes[channel][note].needsDrawing = false;
    activeNotes[channel][note].keyReleaseTime = event.timestamp;
    activeNotes[channel][note].keyIsPressed = false;

    queue_push(event);
    textureNeedsUpdate = true;
}

void notes_per_second(uint64_t nps) {
    notesPerSecond = nps;
    printf("Renderer got: %lu\n", nps);
}

// Apply smoothing to delta time to prevent stuttering
inline __attribute__((always_inline)) static float smooth_delta_time(float dt) {
    // Simple exponential smoothing
    const float alpha = 0.2f;  // Smoothing factor (lower = more smoothing)
    return alpha * dt + (1.0f - alpha) * previousDeltaTime;
}

// Clear the portion of the texture that's scrolled off-screen
inline __attribute__((always_inline)) static void clear_offscreen_texture() {
    BeginTextureMode(scrollTexture);

    // Calculate how much to clear based on time elapsed, with extra width to ensure all notes are cleared
    float clearWidth = deltaTime * scrollSpeed * CLEAR_WIDTH_MULTIPLIER;

    // Make sure we clear at least 5 pixels to avoid missing thin notes
    if (clearWidth < 5.0f) clearWidth = 5.0f;

    // Calculate the position to clear (just behind the current view)
    float clearX = fmodf(scrollOffset - clearWidth, SCROLL_TEXTURE_WIDTH);
    if (clearX < 0) clearX += SCROLL_TEXTURE_WIDTH;

    // Clear that section with a little extra width to ensure complete clearing
    DrawRectangle(clearX, 0, clearWidth + 5, screenHeight, BLACK);

    // Also clear a small area at the texture wrap point if needed
    if (clearX + clearWidth > SCROLL_TEXTURE_WIDTH) {
        float wrapClearWidth = clearX + clearWidth - SCROLL_TEXTURE_WIDTH;
        DrawRectangle(0, 0, wrapClearWidth + 5, screenHeight, BLACK);
    }

    EndTextureMode();
    lastClearTime = globalTime;
}

// Draw/update actively sounding notes
inline __attribute__((always_inline)) static void update_active_notes() {
    BeginTextureMode(scrollTexture);

    // Calculate the current right edge position in the texture
    float currentRightEdge = fmodf(scrollOffset + screenWidth, SCROLL_TEXTURE_WIDTH);

    // For each active note, extend it to the current time
    for (int c = 0; c < MAX_CHANNELS; c++) {
        for (int n = 0; n < MAX_KEYS; n++) {
            if (activeNotes[c][n].isActive) {
                float y = get_note_y(n);

                if (activeNotes[c][n].needsDrawing) {
                    activeNotes[c][n].startX = currentRightEdge;
                    activeNotes[c][n].needsDrawing = false;
                }

                float startX = activeNotes[c][n].startX;

                // Don't draw notes with invalid startX
                // if (startX <= 0) continue;
                if (startX < 0) startX = currentRightEdge;  // Default to now if missing

                // Calculate the current note length in pixels
                float endX = currentRightEdge;

                // Handle wrap-around cases
                if (endX < startX) {
                    // First, draw from startX to end of texture
                    DrawRectangle(startX, y - NOTE_HEIGHT, SCROLL_TEXTURE_WIDTH - startX, NOTE_HEIGHT,
                        get_note_color(c));

                    // Then draw from beginning of texture to endX
                    DrawRectangle(0, y - NOTE_HEIGHT, endX, NOTE_HEIGHT,
                        get_note_color(c));
                } else {
                    // Normal case (no wrap-around)
                    DrawRectangle(startX, y - NOTE_HEIGHT, endX - startX, NOTE_HEIGHT,
                        get_note_color(c));
                }
            }
        }
    }

    EndTextureMode();
}

inline __attribute__((always_inline)) static void update_texture() {
    clear_offscreen_texture();

    float currentRightEdge = fmodf(scrollOffset + screenWidth, SCROLL_TEXTURE_WIDTH);

    BeginTextureMode(scrollTexture);

    MidiEvent event;
    while (queue_pop(&event)) {
        float y = get_note_y(event.note);

        if (event.isNoteOn) {
            activeNotes[event.channel][event.note].startX = currentRightEdge;
            activeNotes[event.channel][event.note].needsDrawing = false;
        } else {
            float startX = activeNotes[event.channel][event.note].startX;

            // Only draw if we have a valid startX (might not if note_on was missed)
            if (startX > 0) {
                float endX = currentRightEdge;

                Color noteColor = get_note_color(event.channel);

                // Handle wrap-around cases
                if (endX < startX) {
                    // Draw from startX to end of texture
                    DrawRectangle(startX, y - NOTE_HEIGHT, SCROLL_TEXTURE_WIDTH - startX, NOTE_HEIGHT, noteColor);

                    // Draw from beginning of texture to endX
                    DrawRectangle(0, y - NOTE_HEIGHT, endX, NOTE_HEIGHT, noteColor);
                } else {
                    // Normal case (no wrap-around)
                    DrawRectangle(startX, y - NOTE_HEIGHT, endX - startX, NOTE_HEIGHT, noteColor);
                }
            }
        }
    }

    EndTextureMode();

    // Update active notes (extend them to current time)
    update_active_notes();

    drawnTime = globalTime;
    textureNeedsUpdate = false;
}

// Draw the piano keyboard with animations for pressed keys
static void draw_animated_keyboard() {
    const int keyboardWidth = KEYBOARD_WIDTH;
    DrawRectangle(screenWidth - keyboardWidth, 0, keyboardWidth, screenHeight, DARKGRAY);

    for (int note = 0; note < MAX_KEYS; note++) {
        int noteType = note % 12;
        float y = get_note_y_piano(note);
        bool isBlackKey = (noteType == 1 || noteType == 3 || noteType == 6 || noteType == 8 || noteType == 10);

        // Check if any channel has this note active (for animation)
        float maxAlpha = 0.0f;
        int activeChannel = -1;

        for (int c = 0; c < MAX_CHANNELS; c++) {
            float alpha = get_key_animation_alpha(
                activeNotes[c][note].keyPressTime,
                activeNotes[c][note].keyReleaseTime,
                activeNotes[c][note].keyIsPressed,
                globalTime
            );

            if (alpha > maxAlpha) {
                maxAlpha = alpha;
                activeChannel = c;
            }
        }

        // If key is active or recently released, draw an animation overlay
        if (maxAlpha > 0.0f && activeChannel >= 0) {
            Color keyColor = get_note_color(activeChannel);

            // Modify alpha of the key color
            keyColor.a = 255 * maxAlpha;

            DrawRectangle(
                screenWidth - keyboardWidth,
                y - NOTE_HEIGHT,
                keyboardWidth,
                NOTE_HEIGHT,
                ColorAlpha(keyColor, 0.7f * maxAlpha)
            );
        }

        // Draw black key overlay after white key animation
        if (isBlackKey) {
            // Make sure black keys are always drawn on top with their base color
            DrawRectangle(screenWidth - keyboardWidth / 2, y - NOTE_HEIGHT, keyboardWidth / 2, NOTE_HEIGHT, BLACK);

            // If the black key is active, add the animation on top
            if (maxAlpha > 0.0f && activeChannel >= 0) {
                Color keyColor = get_note_color(activeChannel);
                DrawRectangle(
                    screenWidth - keyboardWidth / 2,
                    y - NOTE_HEIGHT,
                    keyboardWidth / 2,
                    NOTE_HEIGHT,
                    ColorAlpha(keyColor, 0.8f * maxAlpha)
                );
            }
        }

        // Draw C note labels
        if (noteType == 0) {
            DrawText(TextFormat("C%d", note / 12 - 1), screenWidth - keyboardWidth + 2, y - NOTE_HEIGHT - 8, 10, GRAY);
        }
    }
}

static void* midi_thread(void* arg) {
    char* midiPath = (char*)arg;
    timeOffset = GetTime();
    PlayMIDI(midiPath, note_on, note_off, notes_per_second);
    return NULL;
}

int main(const int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <midi_file>\n", argv[0]);
        return 1;
    }

    init_event_queue();
    InitWindow(screenWidth, screenHeight, "Piano Roll Thingy");
    SetTargetFPS(144);

    // Create a persistent scroll texture
    scrollTexture = LoadRenderTexture(SCROLL_TEXTURE_WIDTH, screenHeight);
    BeginTextureMode(scrollTexture);
    ClearBackground(BLACK);
    EndTextureMode();

    pthread_t midiThread;
    pthread_create(&midiThread, NULL, midi_thread, argv[1]);

    double currentTime = GetTime() - timeOffset;
    globalTime = currentTime;
    lastClearTime = currentTime;
    previousDeltaTime = 1.0 / 60.0;

    while (!WindowShouldClose()) {
        currentTime = GetTime() - timeOffset;
        float rawDeltaTime = currentTime - globalTime;

        // Apply smoothing to delta time to try to prevent stuttering
        deltaTime = smooth_delta_time(rawDeltaTime);
        previousDeltaTime = deltaTime; // Store for next frame's smoothing

        globalTime = currentTime;

        // Always update the texture
        update_texture();

        // Advance the scroll offset - use smoothed delta time for consistent scrolling
        scrollOffset += deltaTime * scrollSpeed;
        if (scrollOffset >= SCROLL_TEXTURE_WIDTH) {
            scrollOffset = fmodf(scrollOffset, SCROLL_TEXTURE_WIDTH);
        }

        BeginDrawing();
        ClearBackground(BLACK);

        // Draw the scroll texture as background
        // Display from right to left
        Rectangle source = { scrollOffset, 0, screenWidth, screenHeight };
        Rectangle dest = { 0, 0, screenWidth, screenHeight };
        DrawTexturePro(scrollTexture.texture, source, dest, (Vector2){ 0, 0 }, 0.0f, WHITE);

        // Overlay a grid for the piano roll
        for (int note = 0; note < MAX_KEYS; note++) {
            float y = get_note_y(note);
            Color lineColor = (note % 12 == 0) ? (Color){255, 255, 255, 255} : (Color){50, 50, 50, 255};
            DrawLine(0, y, screenWidth, y, lineColor);
        }

        // Draw the animated piano keyboard
        draw_animated_keyboard();

        DrawRectangle(5, 5, 300, 60, (Color){ 0, 0, 0, 160 }); // semi-transparent black background
        DrawFPS(10, 10);
        DrawText(TextFormat("Notes per second: %lu", notesPerSecond), 10, 30, 20, WHITE);
        EndDrawing();
    }

    UnloadRenderTexture(scrollTexture);
    CloseWindow();
    return 0;
}