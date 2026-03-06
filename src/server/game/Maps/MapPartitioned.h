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

#ifndef ACORE_MAP_PARTITIONED_H
#define ACORE_MAP_PARTITIONED_H

#include "Map.h"
#include <mutex>
#include <vector>

class MapPartitioned;

/**
 * MapPartition represents one geographic subdivision of a continent map.
 *
 * It covers a specific range of grid coordinates and independently processes
 * all entities within that range. The MapUpdater thread pool can run multiple
 * partitions in parallel, spreading the per-tick workload across CPU cores.
 *
 * Entities that move outside a partition's grid bounds are queued for transfer
 * to the adjacent partition. This transfer is processed after all partition
 * updates complete (in MapPartitioned::DelayedUpdate), guaranteeing that no
 * entity is updated twice or missed within a single tick.
 *
 * Shared systems (transports, zone weather/music/light) are owned by the
 * parent MapPartitioned. Partitions delegate reads and writes for these
 * systems to the parent so that all partitions present a consistent view.
 *
 * Known limitation (v1): Entities near a partition boundary cannot see or
 * directly interact with entities in the adjacent partition. This is because
 * each partition manages its own grid/cell structure. Cross-partition
 * interaction support is planned for a future iteration.
 */
class MapPartition : public Map
{
    friend class MapPartitioned;

public:
    MapPartition(uint32 mapId, uint32 partitionId, MapPartitioned* parent,
                 uint32 gridXMin, uint32 gridXMax,
                 uint32 gridYMin, uint32 gridYMax);
    ~MapPartition() override = default;

    // Map overrides
    bool AddPlayerToMap(Player*) override;
    void RemovePlayerFromMap(Player*, bool) override;
    void AfterPlayerUnlinkFromMap() override;
    void InitVisibilityDistance() override;
    void UnloadAll() override;

    // Relocation overrides: detect when a player crosses partition boundary
    void PlayerRelocation(Player*, float x, float y, float z, float o) override;
    void CreatureRelocation(Creature*, float x, float y, float z, float o) override;

    // Transport overrides: delegate to parent's transport container so that
    // continent boats/zeppelins (stored in MapPartitioned._transports) are
    // visible to players that reside in this partition.
    void SendInitTransports(Player* player) override;
    void SendRemoveTransports(Player* player) override;

    // Zone dynamic info overrides: propagate explicit weather/music/light
    // changes to ALL sibling partitions so the continent presents a consistent
    // zone state regardless of which partition a player is in.
    void SetZoneMusic(uint32 zoneId, uint32 musicId) override;
    void SetZoneWeather(uint32 zoneId, WeatherState weatherId, float weatherGrade) override;
    void SetZoneOverrideLight(uint32 zoneId, uint32 lightId, Milliseconds fadeInTime) override;

    /**
     * Returns true if world position (x, y) falls within this partition's grid range.
     */
    bool ContainsPosition(float x, float y) const;

    /**
     * Returns true if grid coordinate (gridX, gridY) is within this partition's range.
     */
    bool ContainsGrid(uint32 gridX, uint32 gridY) const;

    MapPartitioned* GetPartitionedParent() const { return _parent; }
    uint32 GetPartitionId() const { return _partitionId; }

    [[nodiscard]] bool IsPartition() const override { return true; }

private:
    MapPartitioned* _parent;
    uint32 _partitionId;
    uint32 _gridXMin;
    uint32 _gridXMax;
    uint32 _gridYMin;
    uint32 _gridYMax;
};

/**
 * MapPartitioned is a continent map subdivided into parallel MapPartition sub-maps.
 *
 * It is created in place of the normal Map for non-instanced, non-battleground
 * continent maps when MapPartitioning.Enable = 1. It acts as a container
 * (analogous to MapInstanced for dungeon instances) and coordinates updates
 * across all child partitions.
 *
 * Each partition is scheduled as an independent task in the MapUpdater thread
 * pool so that they run in parallel when MapUpdate.Threads > 1.
 *
 * Entity transfers (entities crossing partition boundaries) are processed
 * sequentially in DelayedUpdate(), after all partition updates have finished.
 */
class MapPartitioned : public Map
{
    friend class MapMgr;

public:
    using PartitionMap = std::unordered_map<uint32, MapPartition*>;

    /**
     * @param id             Map ID (e.g. 0 for Eastern Kingdoms)
     * @param numPartitions  Number of equal-sized partitions along the Y axis (>= 2)
     */
    MapPartitioned(uint32 id, uint32 numPartitions);
    ~MapPartitioned() override;

    // Map overrides
    void Update(uint32 diff, uint32 s_diff, bool thread = true) override;
    void DelayedUpdate(uint32 diff) override;
    void UnloadAll() override;
    void InitVisibilityDistance() override;

    bool AddPlayerToMap(Player*) override;
    void RemovePlayerFromMap(Player*, bool) override;

    // Player iteration: override so that callers who get the base MapPartitioned
    // (e.g. via sMapMgr->FindBaseMap) still reach all continent players.
    void DoForAllPlayers(std::function<void(Player*)> exec) override;
    void PlayDirectSoundToMap(uint32 soundId, uint32 zoneId = 0) override;

    /**
     * Returns the partition whose grid range contains (x, y), or nullptr if
     * no valid partition is found.
     */
    MapPartition* GetPartitionForPosition(float x, float y) const;

    /**
     * Returns the partition with the given 0-based ID, or nullptr.
     */
    MapPartition* GetPartitionById(uint32 id) const;

    PartitionMap& GetPartitions() { return _partitions; }
    [[nodiscard]] PartitionMap const& GetPartitions() const { return _partitions; }
    [[nodiscard]] uint32 GetNumPartitions() const { return static_cast<uint32>(_partitions.size()); }

    [[nodiscard]] bool IsPartitioned() const override { return true; }

    /**
     * Thread-safe: queue a player for transfer to the partition that covers
     * (newX, newY). The transfer is executed in DelayedUpdate().
     */
    void QueuePlayerTransfer(Player* player, float newX, float newY, float newZ, float newO);

    /**
     * Thread-safe: queue a creature for transfer to the partition covering
     * (newX, newY). The transfer is executed in DelayedUpdate().
     *
     * Note: Creature cross-partition transfers are not implemented in v1.
     * This queue is accepted but not processed.
     */
    void QueueCreatureTransfer(Creature* creature, float newX, float newY, float newZ, float newO);

private:
    void CreatePartitions(uint32 numPartitions);
    void ProcessPendingTransfers();

    /**
     * Perform a transparent player partition transfer without sending
     * map-enter/leave packets to the client.
     */
    void TransferPlayerToPartition(Player* player, MapPartition* dest,
                                   float x, float y, float z, float o);

    struct PlayerTransfer
    {
        Player* player;
        float x, y, z, o;
    };

    struct CreatureTransfer
    {
        Creature* creature;
        float x, y, z, o;
    };

    PartitionMap _partitions;
    uint32 _numPartitions;

    std::mutex _transferMutex;
    std::vector<PlayerTransfer> _pendingPlayerTransfers;
    std::vector<CreatureTransfer> _pendingCreatureTransfers;
};

#endif // ACORE_MAP_PARTITIONED_H
