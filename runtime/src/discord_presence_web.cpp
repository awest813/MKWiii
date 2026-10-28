// Web build: there is no local Discord client to reach (its IPC is a Unix socket or named pipe),
// so the /dev/dolphin Discord contract is accepted and dropped.
#include "discord_presence.h"

namespace DiscordPresence {

void Initialize(const std::string&, const std::string&) {}
void SetClient(const std::string&) {}
void SetActivity(Activity) {}
void Reset() {}
void Shutdown() {}

} // namespace DiscordPresence
