#ifndef BPFTIME_DAEMON_HPP
#define BPFTIME_DAEMON_HPP

#include "daemon_config.hpp"

namespace bpftime
{
int start_daemon(struct daemon_config env);
} // namespace bpftime

#endif // BPFTIME_DAEMON_HPP