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
#include "Player.h"
#include "Vehicle.h"

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

void MapPartitioned::QueuePlayerTransfer(Player* player,
                                          float newX, float newY,
                                          float newZ, float newO)
{
    std::lock_guard<std::mutex> guard(_transferMutex);
    _pendingPlayerTransfers.push_back({player, newX, newY, newZ, newO});
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

    // 1. Remove from the current partition's grid.
    if (player->IsInGrid())
        player->RemoveFromGrid();

    // 2. Re-link the player's map reference to the destination partition.
    //    This updates the partition's player list (m_mapRefMgr).
    player->GetMapRef().link(dest, player);

    // 3. Update the player's map pointer.
    player->SetMap(dest);

    // 4. Relocate to the new position.
    player->Relocate(x, y, z, o);
    if (player->IsVehicle())
        player->GetVehicleKit()->RelocatePassengers();
    player->UpdatePositionData();

    // 5. Ensure the relevant grids are loaded in the destination partition.
    dest->LoadGridsInRange(*player, MAX_VISIBILITY_DISTANCE);

    // 6. Add player to the destination partition's grid.
    CellCoord cellCoord = Acore::ComputeCellCoord(x, y);
    if (cellCoord.IsCoordValid())
    {
        Cell newCell(cellCoord);
        dest->EnsureGridCreated(GridCoord(newCell.GridX(), newCell.GridY()));
        dest->AddToGrid(player, newCell);   // allowed: MapPartitioned is friend of MapPartition
    }

    // 7. Update visibility from the new position.
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
        Player* player = t.player;
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
