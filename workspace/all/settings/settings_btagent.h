#ifndef SETTINGS_BTAGENT_H
#define SETTINGS_BTAGENT_H

// Start the BT pairing agent (registers D-Bus agent, enables pairable)
void btagent_start(void);

// Stop the BT pairing agent (unregisters D-Bus agent, disables pairable)
void btagent_stop(void);

#endif // SETTINGS_BTAGENT_H
