/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MapPartitioned.h"
#include "Creature.h"
#include "GridDefines.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Vehicle.h"

// Sentinel zone value matching the MAP_INVALID_ZONE #define in Map.cpp.
static constexpr uint32 PARTITION_INVALID_ZONE = 0xFFFFFFFF;

// ---------------------------------------------------------------------------
// MapPartition
// ---------------------------------------------------------------------------

MapPartition::MapPartition(uint32 mapId, uint32 partitionId, MapPartitioned* parent,
                           uint32 gridXMin, uint32 gridXMax,
                           uint32 gridYMin, uint32 gridYMax)
    : Map(mapId, 0, REGULAR_DIFFICULTY, parent),
      _parent(parent),
      _partitionId(partitionId),
      _gridXMin(gridXMin),
      _gridXMax(gridXMax),
      _gridYMin(gridYMin),
      _gridYMax(gridYMax)
{
    ASSERT(parent);
    ASSERT(gridXMin <= gridXMax);
    ASSERT(gridYMin <= gridYMax);
}

bool MapPartition::ContainsGrid(uint32 gridX, uint32 gridY) const
{
    return gridX >= _gridXMin && gridX <= _gridXMax &&
           gridY >= _gridYMin && gridY <= _gridYMax;
}

bool MapPartition::ContainsPosition(float x, float y) const
{
    GridCoord gc = Acore::ComputeGridCoord(x, y);
    if (!gc.IsCoordValid())
        return false;
    return ContainsGrid(gc.x_coord, gc.y_coord);
}

bool MapPartition::AddPlayerToMap(Player* player)
{
    return Map::AddPlayerToMap(player);
}

void MapPartition::RemovePlayerFromMap(Player* player, bool remove)
{
    Map::RemovePlayerFromMap(player, remove);
}

void MapPartition::AfterPlayerUnlinkFromMap()
{
    Map::AfterPlayerUnlinkFromMap();
}

void MapPartition::InitVisibilityDistance()
{
    Map::InitVisibilityDistance();
}

void MapPartition::UnloadAll()
{
    Map::UnloadAll();
}

void MapPartition::PlayerRelocation(Player* player, float x, float y, float z, float o)
{
    GridCoord newGrid = Acore::ComputeGridCoord(x, y);
    if (newGrid.IsCoordValid() && !ContainsGrid(newGrid.x_coord, newGrid.y_coord))
    {
        // Player has moved into a different partition – queue for transfer.
        // The transfer is processed after all partition updates complete.
        _parent->QueuePlayerTransfer(player, x, y, z, o);
        return;
    }
    Map::PlayerRelocation(player, x, y, z, o);
}

void MapPartition::CreatureRelocation(Creature* creature, float x, float y, float z, float o)
{
    // Creatures are always updated in their source partition even if they
    // temporarily stray into an adjacent partition's grid range.
    // Cross-partition creature transfers are a known limitation of v1.
    Map::CreatureRelocation(creature, x, y, z, o);
}

// ---------------------------------------------------------------------------
// Transport overrides
// ---------------------------------------------------------------------------

void MapPartition::SendInitTransports(Player* player)
{
    // The continent transports (boats/zeppelins) live in the parent
    // MapPartitioned._transports. Delegate to the parent so that every player
    // who joins any partition receives the correct transport update packets.
    _parent->Map::SendInitTransports(player);
}

void MapPartition::SendRemoveTransports(Player* player)
{
    // Mirror of SendInitTransports: send remove packets for all transports
    // owned by the parent.
    _parent->Map::SendRemoveTransports(player);
}

// ---------------------------------------------------------------------------
// Zone dynamic info (weather/music/light) overrides
// ---------------------------------------------------------------------------

void MapPartition::SetZoneMusic(uint32 zoneId, uint32 musicId)
{
    // Enqueue on the parent so the change is applied to ALL partitions in
    // MapPartitioned::DelayedUpdate (after all parallel updates finish).
    // Direct mutation of sibling partitions' _zoneDynamicInfo from a worker
    // thread would race with their concurrent Map::Update execution.
    _parent->EnqueueZoneMusic(zoneId, musicId);
}

void MapPartition::SetZoneWeather(uint32 zoneId, WeatherState weatherId, float weatherGrade)
{
    // See SetZoneMusic for rationale.
    _parent->EnqueueZoneWeather(zoneId, weatherId, weatherGrade);
}

void MapPartition::SetZoneOverrideLight(uint32 zoneId, uint32 lightId, Milliseconds fadeInTime)
{
    // See SetZoneMusic for rationale.
    _parent->EnqueueZoneOverrideLight(zoneId, lightId, fadeInTime);
}

// ---------------------------------------------------------------------------
// MapPartitioned
// ---------------------------------------------------------------------------

MapPartitioned::MapPartitioned(uint32 id, uint32 numPartitions)
    : Map(id, 0, REGULAR_DIFFICULTY),
      _numPartitions(numPartitions)
{
    ASSERT(numPartitions >= 2);
    CreatePartitions(numPartitions);
}

MapPartitioned::~MapPartitioned()
{
    for (auto& [id, partition] : _partitions)
        delete partition;
    _partitions.clear();
}

void MapPartitioned::CreatePartitions(uint32 numPartitions)
{
    // Divide the full grid range [0, MAX_NUMBER_OF_GRIDS) along the Y axis into
    // numPartitions equal-sized slices. The last slice absorbs any remainder.
    const uint32 totalGrids = MAX_NUMBER_OF_GRIDS;
    const uint32 gridsPerPartition = totalGrids / numPartitions;

    for (uint32 i = 0; i < numPartitions; ++i)
    {
        uint32 gridYMin = i * gridsPerPartition;
        uint32 gridYMax = (i == numPartitions - 1)
                              ? (totalGrids - 1)
                              : ((i + 1) * gridsPerPartition - 1);

        auto* partition = new MapPartition(GetId(), i, this,
                                           0, totalGrids - 1,
                                           gridYMin, gridYMax);
        _partitions[i] = partition;

        LOG_DEBUG("maps", "MapPartitioned: Created partition {} for map {} (gridY {}-{})",
                  i, GetId(), gridYMin, gridYMax);
    }
}

void MapPartitioned::InitVisibilityDistance()
{
    for (auto& [id, partition] : _partitions)
        partition->InitVisibilityDistance();
}

MapPartition* MapPartitioned::GetPartitionForPosition(float x, float y) const
{
    GridCoord gc = Acore::ComputeGridCoord(x, y);
    if (!gc.IsCoordValid())
        return nullptr;

    for (auto& [id, partition] : _partitions)
    {
        if (partition->ContainsGrid(gc.x_coord, gc.y_coord))
            return partition;
    }
    return nullptr;
}

MapPartition* MapPartitioned::GetPartitionById(uint32 id) const
{
    auto it = _partitions.find(id);
    return (it != _partitions.end()) ? it->second : nullptr;
}

bool MapPartitioned::AddPlayerToMap(Player* player)
{
    MapPartition* partition = GetPartitionForPosition(
        player->GetPositionX(), player->GetPositionY());

    if (!partition)
    {
        // Fallback: use the first partition if position is unmapped.
        partition = GetPartitionById(0);
        ASSERT(partition);
    }

    player->SetMap(partition);
    return partition->AddPlayerToMap(player);
}

void MapPartitioned::RemovePlayerFromMap(Player* player, bool remove)
{
    // The player's current map is the partition – delegate to it.
    if (Map* playerMap = player->FindMap())
    {
        if (MapPartition* partition = playerMap->ToMapPartition())
        {
            partition->RemovePlayerFromMap(player, remove);
            return;
        }
    }
    // Fallback: search all partitions.
    for (auto& [id, partition] : _partitions)
    {
        for (auto it = partition->GetPlayers().begin();
             it != partition->GetPlayers().end(); ++it)
        {
            if (it->GetSource() == player)
            {
                partition->RemovePlayerFromMap(player, remove);
                return;
            }
        }
    }
}

void MapPartitioned::Update(uint32 diff, uint32 s_diff, bool /*thread*/)
{
    // Update the parent map itself (handles transport updates, base map cleanup).
    Map::Update(diff, s_diff, false);

    // Schedule each partition for independent parallel update via the thread pool.
    for (auto& [id, partition] : _partitions)
    {
        if (sMapMgr->GetMapUpdater()->activated())
            sMapMgr->GetMapUpdater()->schedule_update(*partition, diff, s_diff);
        else
            partition->Update(diff, s_diff);
    }
}

void MapPartitioned::DelayedUpdate(uint32 diff)
{
    // Run delayed visibility/relocation updates for each partition.
    for (auto& [id, partition] : _partitions)
        partition->DelayedUpdate(diff);

    // All partition updates have completed; now process pending cross-partition
    // player transfers sequentially, which is safe.
    ProcessPendingTransfers();

    // Apply zone music/weather/light changes queued from partition worker
    // threads.  Running here (after MapUpdater::wait()) guarantees that all
    // partition threads have finished, so writing to their _zoneDynamicInfo is
    // safe and race-free.
    ApplyPendingZoneChanges();

    Map::DelayedUpdate(diff);
}

void MapPartitioned::UnloadAll()
{
    for (auto& [id, partition] : _partitions)
        partition->UnloadAll();

    for (auto& [id, partition] : _partitions)
        delete partition;
    _partitions.clear();

    Map::UnloadAll();
}

void MapPartitioned::DoForAllPlayers(std::function<void(Player*)> exec)
{
    for (auto& [id, partition] : _partitions)
        partition->Map::DoForAllPlayers(exec);
}

void MapPartitioned::PlayDirectSoundToMap(uint32 soundId, uint32 zoneId)
{
    for (auto& [id, partition] : _partitions)
        partition->Map::PlayDirectSoundToMap(soundId, zoneId);
}

void MapPartitioned::QueuePlayerTransfer(Player* player,
                                          float newX, float newY,
                                          float newZ, float newO)
{
    // Store the player's GUID rather than a raw pointer.  The player might
    // disconnect and be deleted between QueuePlayerTransfer and
    // ProcessPendingTransfers; the GUID lets us safely check for liveness.
    std::lock_guard<std::mutex> guard(_transferMutex);
    _pendingPlayerTransfers.push_back({player->GetGUID(), newX, newY, newZ, newO});
}

void MapPartitioned::QueueCreatureTransfer(Creature* creature,
                                            float newX, float newY,
                                            float newZ, float newO)
{
    std::lock_guard<std::mutex> guard(_transferMutex);
    _pendingCreatureTransfers.push_back({creature, newX, newY, newZ, newO});
}

void MapPartitioned::TransferPlayerToPartition(Player* player,
                                                MapPartition* dest,
                                                float x, float y,
                                                float z, float o)
{
    // Light-weight partition transfer – no map-enter/leave packets are sent
    // to the client because partition boundaries are transparent to players.

    // 0. Capture the source partition before relinking.
    MapPartition* src = player->GetMap() ? player->GetMap()->ToMapPartition() : nullptr;

    // 1. Remove from the current partition's grid.
    if (player->IsInGrid())
        player->RemoveFromGrid();

    // 2. Decrement zone player count on the source partition.
    if (src)
        src->UpdatePlayerZoneStats(player->GetZoneId(), PARTITION_INVALID_ZONE);

    // 3. Re-link the player's map reference to the destination partition.
    //    This updates the partition's player list (m_mapRefMgr).
    player->GetMapRef().link(dest, player);

    // 4. Update the player's map pointer.
    player->SetMap(dest);

    // 5. Relocate to the new position.
    player->Relocate(x, y, z, o);
    if (player->IsVehicle())
        player->GetVehicleKit()->RelocatePassengers();
    player->UpdatePositionData();

    // 6. Ensure the relevant grids are loaded in the destination partition.
    dest->LoadGridsInRange(*player, MAX_VISIBILITY_DISTANCE);

    // 7. Add player to the destination partition's grid.
    CellCoord cellCoord = Acore::ComputeCellCoord(x, y);
    if (cellCoord.IsCoordValid())
    {
        Cell newCell(cellCoord);
        dest->EnsureGridCreated(GridCoord(newCell.GridX(), newCell.GridY()));
        dest->AddToGrid(player, newCell);   // allowed: MapPartitioned is friend of MapPartition
    }

    // 8. Increment zone player count on the destination partition (zone ID may
    //    have changed after UpdatePositionData refreshed the zone).
    dest->UpdatePlayerZoneStats(PARTITION_INVALID_ZONE, player->GetZoneId());

    // 9. Update visibility from the new position.
    player->UpdateObjectVisibility(false);
}

void MapPartitioned::ProcessPendingTransfers()
{
    // Swap under lock so partitions can keep queuing without stalling.
    std::vector<PlayerTransfer>   playerTransfers;
    std::vector<CreatureTransfer> creatureTransfers;
    {
        std::lock_guard<std::mutex> guard(_transferMutex);
        playerTransfers.swap(_pendingPlayerTransfers);
        creatureTransfers.swap(_pendingCreatureTransfers);
    }

    // --- Player transfers ---
    for (auto& t : playerTransfers)
    {
        // Resolve the stored GUID to a live Player pointer.  The player may
        // have disconnected and been deleted since the transfer was queued.
        Player* player = ObjectAccessor::FindPlayer(t.guid);
        if (!player || !player->IsInWorld())
            continue;

        MapPartition* dest = GetPartitionForPosition(t.x, t.y);
        if (!dest)
        {
            LOG_WARN("maps",
                     "MapPartitioned::ProcessPendingTransfers: No destination partition "
                     "for player {} at ({:.1f}, {:.1f}) on map {}",
                     player->GetGUID().ToString(), t.x, t.y, GetId());
            continue;
        }

        MapPartition* src = player->FindMap() ? player->FindMap()->ToMapPartition() : nullptr;
        if (src == dest)
            continue;

        TransferPlayerToPartition(player, dest, t.x, t.y, t.z, t.o);

        LOG_DEBUG("maps",
                  "MapPartitioned: Transferred player {} from partition {} "
                  "to partition {} on map {}",
                  player->GetGUID().ToString(),
                  src ? src->GetPartitionId() : uint32(-1),
                  dest->GetPartitionId(), GetId());
    }

    // --- Creature transfers ---
    // Creature cross-partition transfers are not implemented in v1.
    // Creatures that wander into a neighboring partition's grid range continue
    // to be processed by their source partition. This is a known limitation.
    (void)creatureTransfers;
}

void MapPartitioned::EnqueueZoneMusic(uint32 zoneId, uint32 musicId)
{
    std::lock_guard<std::mutex> guard(_zoneChangeMutex);
    _pendingZoneChanges.push_back(
        {.type = ZoneChangeType::Music, .zoneId = zoneId, .musicId = musicId});
}

void MapPartitioned::EnqueueZoneWeather(uint32 zoneId, WeatherState weatherId,
                                         float weatherGrade)
{
    std::lock_guard<std::mutex> guard(_zoneChangeMutex);
    _pendingZoneChanges.push_back(
        {.type = ZoneChangeType::Weather, .zoneId = zoneId,
         .weatherId = weatherId, .weatherGrade = weatherGrade});
}

void MapPartitioned::EnqueueZoneOverrideLight(uint32 zoneId, uint32 lightId,
                                               Milliseconds fadeInTime)
{
    std::lock_guard<std::mutex> guard(_zoneChangeMutex);
    _pendingZoneChanges.push_back(
        {.type = ZoneChangeType::OverrideLight, .zoneId = zoneId,
         .lightId = lightId, .fadeInTime = fadeInTime});
}

void MapPartitioned::ApplyPendingZoneChanges()
{
    // Swap under lock so partitions can enqueue new changes next tick.
    std::vector<PendingZoneChange> changes;
    {
        std::lock_guard<std::mutex> guard(_zoneChangeMutex);
        changes.swap(_pendingZoneChanges);
    }

    for (auto const& change : changes)
    {
        for (auto& [id, partition] : _partitions)
        {
            switch (change.type)
            {
                case ZoneChangeType::Music:
                    partition->Map::SetZoneMusic(change.zoneId, change.musicId);
                    break;
                case ZoneChangeType::Weather:
                    partition->Map::SetZoneWeather(change.zoneId, change.weatherId,
                                                   change.weatherGrade);
                    break;
                case ZoneChangeType::OverrideLight:
                    partition->Map::SetZoneOverrideLight(change.zoneId, change.lightId,
                                                         change.fadeInTime);
                    break;
            }
        }
    }
}
