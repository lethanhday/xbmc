/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "PartyModeManager.h"

#include <algorithm>

#include "Application.h"
#include "dialogs/GUIDialogProgress.h"
#include "guilib/GUIWindowManager.h"
#include "GUIUserMessages.h"
#include "interfaces/AnnouncementManager.h"
#include "messaging/helpers/DialogOKHelper.h"
#include "music/MusicDatabase.h"
#include "music/windows/GUIWindowMusicPlaylist.h"
#include "PlayListPlayer.h"
#include "playlists/PlayList.h"
#include "playlists/SmartPlayList.h"
#include "profiles/ProfilesManager.h"
#include "threads/SystemClock.h"
#include "utils/log.h"
#include "utils/Random.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "video/VideoDatabase.h"

using namespace KODI::MESSAGING;
using namespace PLAYLIST;

#define QUEUE_DEPTH       10

CPartyModeManager::CPartyModeManager(void)
{
  m_bIsVideo = false;
  m_bEnabled = false;
  m_strCurrentFilterVideo.clear();
  ClearState();
}

CPartyModeManager::~CPartyModeManager(void) = default;

bool CPartyModeManager::Enable(PartyModeContext context /*= PARTYMODECONTEXT_MUSIC*/, const std::string& strXspPath /*= ""*/)
{
  // Filter using our PartyMode xml file
  CSmartPlaylist playlist;
  std::string partyModePath;
  bool playlistLoaded;

  m_bIsVideo = context == PARTYMODECONTEXT_VIDEO;
  if (!strXspPath.empty()) //if a path to a smartplaylist is supplied use it
    partyModePath = strXspPath;
  else if (m_bIsVideo)
    partyModePath = CProfilesManager::GetInstance().GetUserDataItem("PartyMode-Video.xsp");
  else
    partyModePath = CProfilesManager::GetInstance().GetUserDataItem("PartyMode.xsp");

  playlistLoaded=playlist.Load(partyModePath);

  if ( playlistLoaded )
  {
    m_type = playlist.GetType();
    if (context == PARTYMODECONTEXT_UNKNOWN)
    {
      //get it from the xsp file
      m_bIsVideo = (StringUtils::EqualsNoCase(m_type, "video") ||
                    StringUtils::EqualsNoCase(m_type, "musicvideos") ||
                    StringUtils::EqualsNoCase(m_type, "mixed"));
    }

    if (StringUtils::EqualsNoCase(m_type, "mixed"))
      playlist.SetType("songs");

    if (StringUtils::EqualsNoCase(m_type, "mixed"))
      playlist.SetType("video");

    playlist.SetType(m_type);
  }
  else
  {
    m_CurrentFilterMusic = odb::query<ODBView_Song>();
    m_strCurrentFilterVideo.clear();
    m_type = m_bIsVideo ? "musicvideos" : "songs";
  }

  CGUIDialogProgress* pDialog = g_windowManager.GetWindow<CGUIDialogProgress>(WINDOW_DIALOG_PROGRESS);
  int iHeading = (m_bIsVideo ? 20250 : 20121);
  int iLine0 = (m_bIsVideo ? 20251 : 20123);
  pDialog->SetHeading(CVariant{iHeading});
  pDialog->SetLine(0, CVariant{iLine0});
  pDialog->SetLine(1, CVariant{""});
  pDialog->SetLine(2, CVariant{""});
  pDialog->Open();

  ClearState();
  unsigned int time = XbmcThreads::SystemClockMillis();
  std::vector< std::pair<int,int> > songIDs;
  if (StringUtils::EqualsNoCase(m_type, "songs") ||
      StringUtils::EqualsNoCase(m_type, "mixed"))
  {
    CMusicDatabase db;
    if (db.Open())
    {
      std::set<std::string> playlists;
      if ( playlistLoaded )
        m_CurrentFilterMusic = playlist.GetSongWhereClause(playlists);

      m_iMatchingSongs = (int)db.GetSongIDs(m_CurrentFilterMusic, songIDs);
      if (m_iMatchingSongs < 1 && StringUtils::EqualsNoCase(m_type, "songs"))
      {
        pDialog->Close();
        db.Close();
        OnError(16031, (std::string)"Party mode found no matching songs. Aborting.");
        return false;
      }
    }
    else
    {
      pDialog->Close();
      OnError(16033, (std::string)"Party mode could not open database. Aborting.");
      return false;
    }
    db.Close();
  }

  if (StringUtils::EqualsNoCase(m_type, "musicvideos") ||
      StringUtils::EqualsNoCase(m_type, "mixed"))
  {
    std::vector< std::pair<int,int> > songIDs2;
    CVideoDatabase db;
    if (db.Open())
    {
      std::set<std::string> playlists;
      if ( playlistLoaded )
        m_strCurrentFilterVideo = playlist.GetWhereClause(db, playlists);

      CLog::Log(LOGINFO, "PARTY MODE MANAGER: Registering filter:[%s]", m_strCurrentFilterVideo.c_str());
      m_iMatchingSongs += (int)db.GetMusicVideoIDs(m_strCurrentFilterVideo, songIDs2);
      if (m_iMatchingSongs < 1)
      {
        pDialog->Close();
        db.Close();
        OnError(16031, (std::string)"Party mode found no matching songs. Aborting.");
        return false;
      }
    }
    else
    {
      pDialog->Close();
      OnError(16033, (std::string)"Party mode could not open database. Aborting.");
      return false;
    }
    db.Close();
    songIDs.insert(songIDs.end(),songIDs2.begin(),songIDs2.end());
  }

  // calculate history size
  if (m_iMatchingSongs < 50)
    m_songsInHistory = 0;
  else
    m_songsInHistory = (int)(m_iMatchingSongs/2);
  if (m_songsInHistory > 200)
    m_songsInHistory = 200;

  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Matching songs = %i, History size = %i", m_iMatchingSongs, m_songsInHistory);
  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Party mode enabled!");

  int iPlaylist = m_bIsVideo ? PLAYLIST_VIDEO : PLAYLIST_MUSIC;

  CServiceBroker::GetPlaylistPlayer().ClearPlaylist(iPlaylist);
  CServiceBroker::GetPlaylistPlayer().SetShuffle(iPlaylist, false);
  CServiceBroker::GetPlaylistPlayer().SetRepeat(iPlaylist, PLAYLIST::REPEAT_NONE);

  pDialog->SetLine(0, CVariant{m_bIsVideo ? 20252 : 20124});
  pDialog->Progress();
  // add initial songs
  if (!AddInitialSongs(songIDs))
  {
    pDialog->Close();
    return false;
  }
  CLog::Log(LOGDEBUG, "%s time for song fetch: %u",
            __FUNCTION__, XbmcThreads::SystemClockMillis() - time);

  // start playing
  CServiceBroker::GetPlaylistPlayer().SetCurrentPlaylist(iPlaylist);
  Play(0);

  pDialog->Close();
  // open now playing window
  if (StringUtils::EqualsNoCase(m_type, "songs"))
  {
    if (g_windowManager.GetActiveWindow() != WINDOW_MUSIC_PLAYLIST)
      g_windowManager.ActivateWindow(WINDOW_MUSIC_PLAYLIST);
  }

  // done
  m_bEnabled = true;
  Announce();
  return true;
}

void CPartyModeManager::Disable()
{
  if (!IsEnabled())
    return;
  m_bEnabled = false;
  Announce();
  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Party mode disabled.");
}

void CPartyModeManager::OnSongChange(bool bUpdatePlayed /* = false */)
{
  if (!IsEnabled())
    return;
  Process();
  if (bUpdatePlayed)
    m_iSongsPlayed++;
}

void CPartyModeManager::AddUserSongs(CPlayList& tempList, bool bPlay /* = false */)
{
  if (!IsEnabled())
    return;

  // where do we add?
  int iAddAt = -1;
  if (m_iLastUserSong < 0 || bPlay)
    iAddAt = 1; // under the currently playing song
  else
    iAddAt = m_iLastUserSong + 1; // under the last user added song

  int iNewUserSongs = tempList.size();
  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Adding %i user selected songs at %i", iNewUserSongs, iAddAt);

  int iPlaylist = PLAYLIST_MUSIC;
  if (m_bIsVideo)
    iPlaylist = PLAYLIST_VIDEO;
  CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist).Insert(tempList, iAddAt);

  // update last user added song location
  if (m_iLastUserSong < 0)
    m_iLastUserSong = 0;
  m_iLastUserSong += iNewUserSongs;

  if (bPlay)
    Play(1);
}

void CPartyModeManager::AddUserSongs(CFileItemList& tempList, bool bPlay /* = false */)
{
  if (!IsEnabled())
    return;

  // where do we add?
  int iAddAt = -1;
  if (m_iLastUserSong < 0 || bPlay)
    iAddAt = 1; // under the currently playing song
  else
    iAddAt = m_iLastUserSong + 1; // under the last user added song

  int iNewUserSongs = tempList.Size();
  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Adding %i user selected songs at %i", iNewUserSongs, iAddAt);

  int iPlaylist = PLAYLIST_MUSIC;
  if (m_bIsVideo)
    iPlaylist = PLAYLIST_VIDEO;

  CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist).Insert(tempList, iAddAt);

  // update last user added song location
  if (m_iLastUserSong < 0)
    m_iLastUserSong = 0;
  m_iLastUserSong += iNewUserSongs;

  if (bPlay)
    Play(1);
}

void CPartyModeManager::Process()
{
  ReapSongs();
  MovePlaying();
  AddRandomSongs();
  UpdateStats();
  SendUpdateMessage();
}

bool CPartyModeManager::AddRandomSongs(int iSongs /* = 0 */)
{
  int iPlaylist = PLAYLIST_MUSIC;
  if (m_bIsVideo)
    iPlaylist = PLAYLIST_VIDEO;

  CPlayList& playlist = CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist);
  int iMissingSongs = QUEUE_DEPTH - playlist.size();
  if (iSongs <= 0)
    iSongs = iMissingSongs;
  // distribute between types if mixed
  int iSongsToAdd=iSongs;
  int iVidsToAdd=iSongs;
  if (StringUtils::EqualsNoCase(m_type, "mixed"))
  {
    if (iSongs == 1)
    {
      if (rand() % 10 < 7) // 70 % chance of grabbing a song
        iVidsToAdd = 0;
      else
        iSongsToAdd = 0;
    }
    if (iSongs > 1) // grab 70 % songs, 30 % mvids
    {
      iSongsToAdd = (int).7f*iSongs;
      iVidsToAdd = (int).3f*iSongs;
      while (iSongsToAdd+iVidsToAdd < iSongs) // correct any rounding by adding songs
        iSongsToAdd++;
    }
  }

  // add songs to fill queue
  if (StringUtils::EqualsNoCase(m_type, "songs") ||
      StringUtils::EqualsNoCase(m_type, "mixed"))
  {
    CMusicDatabase database;
    if (database.Open())
    {
      // Method:
      // 1. Grab a random entry from the database using a where clause
      // 2. Iterate on iSongs.

      // Note: At present, this method is faster than the alternative, which is to grab
      // all valid songids, then select a random number of them (as done in AddInitialSongs()).
      // The reason for this is simply the number of songs we are requesting - we generally
      // only want one here.  Any more than about 3 songs and it is more efficient
      // to use the technique in AddInitialSongs.  As it's unlikely that we'll require
      // more than 1 song at a time here, this method is faster.
      
      //Create the where clause for music
      std::vector<int> historyItemsMusic;
      for (unsigned int i = 0; i < m_history.size(); i++)
      {
        if (m_history[i].first == 1)
          historyItemsMusic.push_back(m_history[i].second);
      }
      odb::query<ODBView_Song> objQuery;
      if (!historyItemsMusic.empty())
      {
        objQuery = !odb::query<ODBView_Song>::CODBSong::idSong.in_range(historyItemsMusic.begin(), historyItemsMusic.end());
      }
      
      bool error(false);
      for (int i = 0; i < iSongsToAdd; i++)
      {
        CFileItemPtr item(new CFileItem);
        int songID;
        if (database.GetRandomSong(item.get(), songID, objQuery))
        { // success
          Add(item);
          AddToHistory(1,songID);
        }
        else
        {
          error = true;
          break;
        }
      }

      if (error)
      {
        database.Close();
        OnError(16034, (std::string)"Cannot get songs from database. Aborting.");
        return false;
      }
    }
    else
    {
      OnError(16033, (std::string)"Party mode could not open database. Aborting.");
      return false;
    }
    database.Close();
  }
  if (StringUtils::EqualsNoCase(m_type, "musicvideos") ||
      StringUtils::EqualsNoCase(m_type, "mixed"))
  {
    CVideoDatabase database;
    if (database.Open())
    {
      // Method:
      // 1. Grab a random entry from the database using a where clause
      // 2. Iterate on iSongs.

      // Note: At present, this method is faster than the alternative, which is to grab
      // all valid songids, then select a random number of them (as done in AddInitialSongs()).
      // The reason for this is simply the number of songs we are requesting - we generally
      // only want one here.  Any more than about 3 songs and it is more efficient
      // to use the technique in AddInitialSongs.  As it's unlikely that we'll require
      // more than 1 song at a time here, this method is faster.
      bool error(false);
      for (int i = 0; i < iVidsToAdd; i++)
      {
        //TODO: To do with Music Videos
        /*std::pair<std::string,std::string> whereClause = GetWhereClauseWithHistory();
        CFileItemPtr item(new CFileItem);
        int songID;
        if (database.GetRandomMusicVideo(item.get(), songID, whereClause.second))
        { // success
          Add(item);
          AddToHistory(2,songID);
        }
        else
        {
          error = true;
          break;
        }*/
      }

      if (error)
      {
        database.Close();
        OnError(16034, (std::string)"Cannot get songs from database. Aborting.");
        return false;
      }
    }
    else
    {
      OnError(16033, (std::string)"Party mode could not open database. Aborting.");
      return false;
    }
    database.Close();
  }
  return true;
}

void CPartyModeManager::Add(CFileItemPtr &pItem)
{
  int iPlaylist = m_bIsVideo ? PLAYLIST_VIDEO : PLAYLIST_MUSIC;
  if (pItem->HasMusicInfoTag())
  {
    CMusicDatabase database;
    database.Open();
    database.SetPropertiesForFileItem(*pItem);
  }

  CPlayList& playlist = CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist);
  playlist.Add(pItem);
  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Adding randomly selected song at %i:[%s]", playlist.size() - 1, pItem->GetPath().c_str());
  m_iMatchingSongsPicked++;
}

bool CPartyModeManager::ReapSongs()
{
  int iPlaylist = m_bIsVideo ? PLAYLIST_VIDEO : PLAYLIST_MUSIC;

  // reap any played songs
  int iCurrentSong = CServiceBroker::GetPlaylistPlayer().GetCurrentSong();
  int i=0;
  while (i < CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist).size())
  {
    if (i < iCurrentSong)
    {
      CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist).Remove(i);
      iCurrentSong--;
      if (i <= m_iLastUserSong)
        m_iLastUserSong--;
    }
    else
      i++;
  }

  CServiceBroker::GetPlaylistPlayer().SetCurrentSong(iCurrentSong);
  return true;
}

bool CPartyModeManager::MovePlaying()
{
  // move current song to the top if its not there
  int iCurrentSong = CServiceBroker::GetPlaylistPlayer().GetCurrentSong();
  int iPlaylist = m_bIsVideo ? PLAYLIST_MUSIC : PLAYLIST_VIDEO;

  if (iCurrentSong > 0)
  {
    CLog::Log(LOGINFO,"PARTY MODE MANAGER: Moving currently playing song from %i to 0", iCurrentSong);
    CPlayList &playlist = CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist);
    CPlayList playlistTemp;
    playlistTemp.Add(playlist[iCurrentSong]);
    playlist.Remove(iCurrentSong);
    for (int i=0; i<playlist.size(); i++)
      playlistTemp.Add(playlist[i]);
    playlist.Clear();
    for (int i=0; i<playlistTemp.size(); i++)
      playlist.Add(playlistTemp[i]);
  }
  CServiceBroker::GetPlaylistPlayer().SetCurrentSong(0);
  return true;
}

void CPartyModeManager::SendUpdateMessage()
{
  CGUIMessage msg(GUI_MSG_PLAYLIST_CHANGED, 0, 0);
  g_windowManager.SendThreadMessage(msg);
}

void CPartyModeManager::Play(int iPos)
{
  // move current song to the top if its not there
  CServiceBroker::GetPlaylistPlayer().Play(iPos, "");
  CLog::Log(LOGINFO,"PARTY MODE MANAGER: Playing song at %i", iPos);
  Process();
}

void CPartyModeManager::OnError(int iError, const std::string&  strLogMessage)
{
  // open error dialog
  HELPERS::ShowOKDialogLines(CVariant{257}, CVariant{16030}, CVariant{iError}, CVariant{0});
  CLog::Log(LOGERROR, "PARTY MODE MANAGER: %s", strLogMessage.c_str());
  m_bEnabled = false;
  SendUpdateMessage();
}

int CPartyModeManager::GetSongsPlayed()
{
  if (!IsEnabled())
    return -1;
  return m_iSongsPlayed;
}

int CPartyModeManager::GetMatchingSongs()
{
  if (!IsEnabled())
    return -1;
  return m_iMatchingSongs;
}

int CPartyModeManager::GetMatchingSongsPicked()
{
  if (!IsEnabled())
    return -1;
  return m_iMatchingSongsPicked;
}

int CPartyModeManager::GetMatchingSongsLeft()
{
  if (!IsEnabled())
    return -1;
  return m_iMatchingSongsLeft;
}

int CPartyModeManager::GetRelaxedSongs()
{
  if (!IsEnabled())
    return -1;
  return m_iRelaxedSongs;
}

int CPartyModeManager::GetRandomSongs()
{
  if (!IsEnabled())
    return -1;
  return m_iRandomSongs;
}

PartyModeContext CPartyModeManager::GetType() const
{
  if (!IsEnabled())
    return PARTYMODECONTEXT_UNKNOWN;

  if (m_bIsVideo)
    return PARTYMODECONTEXT_VIDEO;

  return PARTYMODECONTEXT_MUSIC;
}

void CPartyModeManager::ClearState()
{
  m_iLastUserSong = -1;
  m_iSongsPlayed = 0;
  m_iMatchingSongs = 0;
  m_iMatchingSongsPicked = 0;
  m_iMatchingSongsLeft = 0;
  m_iRelaxedSongs = 0;
  m_iRandomSongs = 0;

  m_songsInHistory = 0;
  m_history.clear();
}

void CPartyModeManager::UpdateStats()
{
  m_iMatchingSongsLeft = m_iMatchingSongs - m_iMatchingSongsPicked;
  m_iRandomSongs = m_iMatchingSongsPicked;
  m_iRelaxedSongs = 0;  // unsupported at this stage
}

bool CPartyModeManager::AddInitialSongs(std::vector< std::pair<int,int > > &songIDs)
{
  int iPlaylist = m_bIsVideo ? PLAYLIST_VIDEO : PLAYLIST_MUSIC;

  CPlayList& playlist = CServiceBroker::GetPlaylistPlayer().GetPlaylist(iPlaylist);
  int iMissingSongs = QUEUE_DEPTH - playlist.size();
  if (iMissingSongs > 0)
  {
    // generate iMissingSongs random ids from songIDs
    if (iMissingSongs > (int)songIDs.size())
      return false; // can't do it if we have less songs than we need

    std::vector<std::pair<int,int> > chosenSongIDs;
    GetRandomSelection(songIDs, iMissingSongs, chosenSongIDs);
    std::string sqlWhereMusic = "songview.idSong IN (";
    std::string sqlWhereVideo = "idMVideo IN (";

    for (std::vector< std::pair<int,int> >::iterator it = chosenSongIDs.begin(); it != chosenSongIDs.end(); ++it)
    {
      std::string song = StringUtils::Format("%i,", it->second);
      if (it->first == 1)
        sqlWhereMusic += song;
      if (it->first == 2)
        sqlWhereVideo += song;
    }
    // add songs to fill queue
    CFileItemList items;

    if (sqlWhereMusic.size() > 26)
    {
      sqlWhereMusic[sqlWhereMusic.size() - 1] = ')'; // replace the last comma with closing bracket
      CMusicDatabase database;
      database.Open();
      database.GetSongsFullByWhere("musicdb://songs/", CDatabase::Filter(sqlWhereMusic),
                                   items, SortDescription(), true);
    }
    if (sqlWhereVideo.size() > 19)
    {
      sqlWhereVideo[sqlWhereVideo.size() - 1] = ')'; // replace the last comma with closing bracket
      CVideoDatabase database;
      database.Open();
      database.GetMusicVideosByWhere("videodb://musicvideos/titles/",
                                     CDatabase::Filter(sqlWhereVideo), items);
    }

    m_history = chosenSongIDs;
    items.Randomize(); //randomizing the initial list or they will be in database order
    for (int i = 0; i < items.Size(); i++)
    {
      CFileItemPtr item(items[i]);
      Add(item);
      //! @todo Allow "relaxed restrictions" later?
    }
  }
  return true;
}

void CPartyModeManager::AddToHistory(int type, int songID)
{
  while (m_history.size() >= m_songsInHistory && m_songsInHistory)
    m_history.erase(m_history.begin());
  m_history.push_back(std::make_pair(type,songID));
}

void CPartyModeManager::GetRandomSelection(std::vector< std::pair<int,int> >& in, unsigned int number, std::vector< std::pair<int,int> >& out)
{
  number = std::min(number, (unsigned int)in.size());
  KODI::UTILS::RandomShuffle(in.begin(), in.end());
  out.assign(in.begin(), in.begin() + number);
}

bool CPartyModeManager::IsEnabled(PartyModeContext context /* = PARTYMODECONTEXT_UNKNOWN */) const
{
  if (!m_bEnabled) return false;
  if (context == PARTYMODECONTEXT_VIDEO)
    return m_bIsVideo;
  if (context == PARTYMODECONTEXT_MUSIC)
    return !m_bIsVideo;
  return true; // unknown, but we're enabled
}

void CPartyModeManager::Announce()
{
  if (g_application.m_pPlayer->IsPlaying())
  {
    CVariant data;
    
    data["player"]["playerid"] = CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist();
    data["property"]["partymode"] = m_bEnabled;
    ANNOUNCEMENT::CAnnouncementManager::GetInstance().Announce(ANNOUNCEMENT::Player, "xbmc", "OnPropertyChanged", data);
  }
}
