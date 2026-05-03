#ifndef DISTORTION_H
#define DISTORTION_H

// Initializes the NI-DAQ digital output task and resets the wiper to position 0.
// Call once at startup before any other distortion function.
void distortion_init(void);

// Moves the digipot wiper to match the target volume (0.0 = min, 1.0 = max).
// Converts to a step count (0-99) and steps the wiper from its current position.
void distortion_set_volume(float volume);

// Saves the current wiper position to NVM, releases CS, and cleans up the DAQ task.
// Call on shutdown.
void distortion_store(void);

#endif /* DISTORTION_H */