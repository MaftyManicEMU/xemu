//
// xemu XB.Live integration
//
// Copyright (C) 2026
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <ctime>
#include <mutex>
#include <string>

struct XBLiveServiceState {
    bool session_loaded = false;
    bool signed_in = false;
    bool busy_login = false;
    bool busy_refresh = false;
    bool busy_cloud_push = false;
    bool busy_cloud_pull = false;
    bool presence_active = false;
    bool presence_inflight = false;

    int friends_count = 0;
    int games_count = 0;
    int messages_count = 0;
    int events_count = 0;
    int active_games_count = 0;
    int games_played_count = 0;
    int leaderboard_rank_count = 0;
    int social_inbox_count = 0;
    int conversations_count = 0;
    int messageable_friends_count = 0;
    int friend_requests_count = 0;
    int blocks_count = 0;
    int activity_24h_points = 0;
    int activity_7d_points = 0;
    int achievements_count = 0;
    int gamerscore = 0;

    double total_minutes = 0.0;
    std::time_t last_refresh = 0;

    std::string username;
    std::string email;
    std::string session_key;
    std::string linked_gamertag;
    std::string current_game;
    std::string last_played_game;
    std::string status_message;
    std::string auth_message;
    std::string profile_message;
    std::string cloud_message;
    std::string presence_message;
    std::string xbox_live_profile_sync_status;
    std::string presence_title_id;
    std::string presence_game_name;
};

class XBLiveService {
public:
    static XBLiveService &Get();

    void LoadSession();
    void Shutdown();
    void Login(const std::string &email, const std::string &password);
    void Logout();
    void RefreshProfile(bool refresh_server_cache);
    void PushCloudArchives(const std::string &directory);
    void PullCloudArchives(const std::string &directory);
    void TickPresence();
    void ForcePresenceOffline(const char *reason);

    XBLiveServiceState Snapshot();
    std::string DefaultCloudSavesDirectory();

private:
    XBLiveService() = default;
    XBLiveService(const XBLiveService &) = delete;
    XBLiveService &operator=(const XBLiveService &) = delete;

    std::mutex m_mutex;
    XBLiveServiceState m_state;
    bool m_load_attempted = false;
    bool m_shutdown_pulse_sent = false;
    std::time_t m_next_presence_ping = 0;
};
