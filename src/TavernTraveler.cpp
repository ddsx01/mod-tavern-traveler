/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "ScriptedGossip.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Config.h"
#include "EventProcessor.h"
#include "WorldSession.h"
#include "Chat.h" 
#include "Opcodes.h"
#include "WorldPacket.h"
#include <map>

// Уникальные идентификаторы действий нашей изолированной системы
const uint32 ACTION_OPEN_NONE    = 10000;
const uint32 ACTION_OPEN_DEFAULT = 10001;
const uint32 ACTION_OPEN_TRAVEL  = 10002;
const uint32 ACTION_BACK_TO_MAIN = 10003;
const uint32 ACTION_CLOSE_MENU   = 10004;
const uint32 ACTION_TRAVEL_EXEC  = 100000;

const uint32 SPELL_HEARTHSTONE_VISUAL = 8690;

// Класс события телепортации
class TavernTeleportEvent : public BasicEvent
{
public:
    TavernTeleportEvent(Player* player, uint32 mapId, float x, float y, float z)
        : _player(player), _mapId(mapId), _x(x), _y(y), _z(z) 
    {
        _initPos.Relocate(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    }

    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        if (!_player || !_player->IsInWorld())
            return true;

        if (_player->IsInCombat() || _player->GetExactDistSq(&_initPos) > 0.5f)
        {
            ChatHandler(_player->GetSession()).SendSysMessage("Телепортация прервана!");
            _player->InterruptNonMeleeSpells(false);
            return true;
        }

        _player->InterruptNonMeleeSpells(false);
        _player->TeleportTo(_mapId, _x, _y, _z, _player->GetOrientation());
        
        return true;
    }

private:
    Player* _player;
    uint32 _mapId;
    float _x, _y, _z;
    Position _initPos;
};

// Хук сохранения таверны
class TavernTravelPlayerScript : public PlayerScript
{
public:
    TavernTravelPlayerScript() : PlayerScript("TavernTravelPlayerScript", {
        PLAYERHOOK_ON_SAVE
    }) {}

    void OnPlayerSave(Player* player) override
    {
        uint32 zoneId = player->m_homebindAreaId; 
        if (zoneId == 0) return;

        CharacterDatabase.Execute("REPLACE INTO character_discovered_inns (guid, zone_id, map_id, pos_x, pos_y, pos_z) VALUES ({}, {}, {}, {}, {}, {})",
            player->GetGUID().GetCounter(), zoneId, player->m_homebindMapId, 
            player->m_homebindX, player->m_homebindY, player->m_homebindZ);
    }
};

// Основной скрипт трактирщика
class TavernTravelCreatureScript : public CreatureScript
{
public:
    TavernTravelCreatureScript() : CreatureScript("TavernTravelCreatureScript") {}
    
    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!creature->IsInnkeeper())
            return false;

        if (!sConfigMgr->GetOption<bool>("TavernTravel.Enable", true))
            return false;

        ClearGossipMenuFor(player);

        // 1. Показываем квесты, если они есть у NPC
        // if (creature->IsQuestGiver())
        //    player->PrepareQuestMenu(creature->GetGUID());

        // 2. Создаем "Главное меню", состоящее только из наших кнопок (без PrepareGossipMenu).
        // Это полностью изолирует экран от багов сортировки клиента.
		AddGossipItemFor(player, GOSSIP_ICON_DOT, "...", GOSSIP_SENDER_MAIN, ACTION_OPEN_NONE);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Стандартные услуги (Торговля, Квесты)...", GOSSIP_SENDER_MAIN, ACTION_OPEN_DEFAULT);
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Путешествие до таверны...", GOSSIP_SENDER_MAIN, ACTION_OPEN_TRAVEL);

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
        return true; 
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        // === ЛОГИКА НАШИХ МЕНЮ ===
		
		if (action == ACTION_OPEN_NONE)
		{
            return true;
		}
		
        if (action == ACTION_OPEN_DEFAULT)
        {
            ClearGossipMenuFor(player);
            // Игрок перешел на страницу "Стандартные услуги".
            // ТЕПЕРЬ мы безопасно загружаем оригинальное меню из базы данных.
            // На этом экране нет наших кнопок, поэтому Хэллоуин и торговля будут работать идеально!
            player->PrepareGossipMenu(creature, creature->GetCreatureTemplate()->GossipMenuId, true);
            player->SendPreparedGossip(creature);
            return true;
        }

        if (action == ACTION_BACK_TO_MAIN)
        {
            // Возврат на стартовый экран
            OnGossipHello(player, creature);
            return true;
        }

        if (action == ACTION_CLOSE_MENU)
        {
            CloseGossipMenuFor(player);
            return true;
        }
        
        if (action == ACTION_OPEN_TRAVEL)
        {
            ClearGossipMenuFor(player);
            std::map<uint32, std::string> taverns;

            // Мгновенное сохранение текущего привязанного камня
            if (player->m_homebindAreaId != 0)
            {
                CharacterDatabase.Execute("REPLACE INTO character_discovered_inns (guid, zone_id, map_id, pos_x, pos_y, pos_z) VALUES ({}, {}, {}, {}, {}, {})",
                    player->GetGUID().GetCounter(), player->m_homebindAreaId, player->m_homebindMapId, 
                    player->m_homebindX, player->m_homebindY, player->m_homebindZ);

                AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->m_homebindAreaId);
                if (area) {
                    std::string name = area->area_name[player->GetSession()->GetSessionDbcLocale()];
                    if (!name.empty()) taverns[player->m_homebindAreaId] = name;
                }
            }

            // Загружаем остальные локации из базы
            QueryResult result = CharacterDatabase.Query("SELECT zone_id FROM character_discovered_inns WHERE guid = {}", player->GetGUID().GetCounter());
            if (result)
            {
                do {
                    uint32 zoneId = result->Fetch()[0].Get<uint32>();
                    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
                    if (area) {
                        std::string name = area->area_name[player->GetSession()->GetSessionDbcLocale()];
                        if (!name.empty()) { taverns[zoneId] = name; }
                    }
                } while (result->NextRow());
            }
			
			AddGossipItemFor(player, GOSSIP_ICON_DOT, " ", GOSSIP_SENDER_MAIN, ACTION_OPEN_NONE);
			
            uint32 addedCount = 0;
            for (auto const& pair : taverns)
            {
                AddGossipItemFor(player, GOSSIP_ICON_TAXI, pair.second, GOSSIP_SENDER_MAIN, ACTION_TRAVEL_EXEC + pair.first);
                addedCount++;
            }

            if (addedCount == 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Вы еще не посещали другие таверны.", GOSSIP_SENDER_MAIN, ACTION_CLOSE_MENU);

            AddGossipItemFor(player, GOSSIP_ICON_DOT, "Назад", GOSSIP_SENDER_MAIN, ACTION_BACK_TO_MAIN);
            SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
            return true;
        }

        // Игрок выбрал конкретную таверну
        if (action >= ACTION_TRAVEL_EXEC)
        {
            uint32 targetZoneId = action - ACTION_TRAVEL_EXEC;
            uint32 travelCost = sConfigMgr->GetOption<uint32>("TavernTravel.Cost", 1000); 
            uint32 castTime = sConfigMgr->GetOption<uint32>("TavernTravel.CastTime", 5);

            if (player->GetMoney() < travelCost)
            {
                ChatHandler(player->GetSession()).SendSysMessage("Недостаточно средств! Требуется 10 серебряных.");
                CloseGossipMenuFor(player);
                return true;
            }

            QueryResult result = CharacterDatabase.Query("SELECT map_id, pos_x, pos_y, pos_z FROM character_discovered_inns WHERE guid = {} AND zone_id = {}", 
                player->GetGUID().GetCounter(), targetZoneId);
                
            if (result)
            {
                Field* fields = result->Fetch();
                
                if (travelCost > 0)
                    player->ModifyMoney(-int32(travelCost));
                
                // Чтобы наш модуль не вешал игроку реальный кулдаун на предмет в инвентаре:
                bool hasCooldown = player->HasSpellCooldown(SPELL_HEARTHSTONE_VISUAL);
                player->CastSpell(player, SPELL_HEARTHSTONE_VISUAL, false);
                
                if (!hasCooldown)
                    player->RemoveSpellCooldown(SPELL_HEARTHSTONE_VISUAL, true);
                
                player->m_Events.AddEvent(new TavernTeleportEvent(player, fields[0].Get<uint32>(), fields[1].Get<float>(), fields[2].Get<float>(), fields[3].Get<float>()), 
                    player->m_Events.CalculateTime(castTime * 1000));

                ChatHandler(player->GetSession()).PSendSysMessage("Телепортация начнется через %u сек.", castTime);
            }
            
            CloseGossipMenuFor(player);
            return true;
        }

        // === ОБРАБОТКА СТАНДАРТНОГО МЕНЮ ===
        // Если игрок зашел в "Стандартные услуги" и нажал любую базовую кнопку (Хэллоуин, Торговля), 
        // скрипт возвращает false. Дальше ядро само идеально выполнит нужные действия!
        return false;
    }
};

void AddTavernTravelScripts()
{
    new TavernTravelPlayerScript();
    new TavernTravelCreatureScript();
}