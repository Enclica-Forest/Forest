#include "include/sound.h"
#include "include/screen.h"
#include "include/timer.h"
#include "include/libc/string.h"
#include "include/memory.h"
#include "include/libc/math.h"
#include "include/enhanced_heap.h"

// Test function to verify SB16 PCM sample handling
void test_sb16_pcm_samples() {
    print("[SB16 Test] Starting PCM sample test...\n");
    
    // Test 1: 16-bit stereo sample at 44100Hz
    {
        print("[SB16 Test] Testing 16-bit stereo 44100Hz...\n");
        
        // Create test sine wave samples
        const int duration_ms = 500;
        const int sample_rate = 44100;
        const int frequency = 440; // A4 note
        const int num_samples = (sample_rate * duration_ms) / 1000;
        const int total_samples = num_samples * 2; // stereo
        
        int16_t* samples = (int16_t*)enhanced_heap_alloc(total_samples * sizeof(int16_t), __func__);
        if (!samples) {
            print("[SB16 Test] Failed to allocate test samples\n");
            return;
        }
        
        // Generate stereo sine wave
        for (int i = 0; i < num_samples; i++) {
            float phase = 2.0f * 3.14159f * frequency * i / sample_rate;
            int16_t sample = (int16_t)(16383.0f * sinf(phase));
            samples[i * 2] = sample;      // Left channel
            samples[i * 2 + 1] = sample;  // Right channel
        }
        
        SoundFormat format = {
            .sample_rate = sample_rate,
            .channels = 2,
            .bits_per_sample = 16,
            .signed_samples = true
        };
        
        // Get the active sound driver (should be SB16)
        const SoundDriver* driver = sound_active_driver();
        if (driver && driver->play_pcm) {
            print("[SB16 Test] Playing 16-bit stereo sample...\n");
            bool result = driver->play_pcm((SoundDriver*)driver, (uint8_t*)samples, 
                                         total_samples * sizeof(int16_t), &format);
            if (result) {
                print("[SB16 Test] ✓ 16-bit stereo playback started successfully\n");
            } else {
                print("[SB16 Test] ✗ 16-bit stereo playback failed\n");
            }
        } else {
            print("[SB16 Test] No active sound driver or play_pcm function\n");
        }
        
        enhanced_heap_free(samples, __func__);
        
        // Wait for playback to complete
        uint32_t start = timer_get_ticks();
        while (timer_get_ticks() - start < 50) {
            __asm__ volatile("nop");
        }
    }
    
    // Test 2: 8-bit mono sample at 22050Hz
    {
        print("[SB16 Test] Testing 8-bit mono 22050Hz...\n");
        
        const int duration_ms = 300;
        const int sample_rate = 22050;
        const int frequency = 880; // A5 note
        const int num_samples = (sample_rate * duration_ms) / 1000;
        
        uint8_t* samples = (uint8_t*)enhanced_heap_alloc(num_samples, __func__);
        if (!samples) {
            print("[SB16 Test] Failed to allocate 8-bit test samples\n");
            return;
        }
        
        // Generate mono square wave (simpler for 8-bit)
        for (int i = 0; i < num_samples; i++) {
            int period = sample_rate / frequency;
            samples[i] = ((i / period) % 2) ? 0xFF : 0x00;
        }
        
        SoundFormat format = {
            .sample_rate = sample_rate,
            .channels = 1,
            .bits_per_sample = 8,
            .signed_samples = false
        };
        
        const SoundDriver* driver = sound_active_driver();
        if (driver && driver->play_pcm) {
            print("[SB16 Test] Playing 8-bit mono sample...\n");
            bool result = driver->play_pcm((SoundDriver*)driver, samples, 
                                         num_samples, &format);
            if (result) {
                print("[SB16 Test] ✓ 8-bit mono playback started successfully\n");
            } else {
                print("[SB16 Test] ✗ 8-bit mono playback failed\n");
            }
        }
        
        enhanced_heap_free(samples, __func__);
        
        // Wait for playback to complete
        uint32_t start_8bit = timer_get_ticks();
        while (timer_get_ticks() - start_8bit < 30) {
            __asm__ volatile("nop");
        }
    }
    
    // Test 3: Test beep function with improved implementation
    {
        print("[SB16 Test] Testing beep function...\n");
        sound_beep(1000, 200); // 1kHz beep for 200ms
        uint32_t start_beep = timer_get_ticks();
        while (timer_get_ticks() - start_beep < 25) {
            __asm__ volatile("nop");
        }
        print("[SB16 Test] ✓ Beep test completed\n");
    }
    
    print("[SB16 Test] PCM sample test completed\n");
}