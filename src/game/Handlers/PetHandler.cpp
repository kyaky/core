/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Log.h"
#include "Opcodes.h"
#include "Spell.h"
#include "CreatureAI.h"
#include "Util.h"
#include "Pet.h"
#include "World.h"

void WorldSession::HandlePetAction(WorldPacket& recv_data)
{
    ObjectGuid petGuid;
    uint32 data;
    ObjectGuid targetGuid;
    recv_data >> petGuid;
    recv_data >> data;
    recv_data >> targetGuid;

    uint32 spellid = UNIT_ACTION_BUTTON_ACTION(data);
    uint8 flag = UNIT_ACTION_BUTTON_TYPE(data);             // delete = 0x07 CastSpell = C1

    DETAIL_LOG("HandlePetAction: %s flag is %u, spellid is %u, target %s.", petGuid.GetString().c_str(), uint32(flag), spellid, targetGuid.GetString().c_str());

    // used also for charmed creature/player
    Unit* pCharmedUnit = _player->GetMap()->GetUnit(petGuid);
    if (!pCharmedUnit)
    {
        sLog.outError("HandlePetAction: %s not exist.", petGuid.GetString().c_str());
        return;
    }

    if (GetPlayer()->GetObjectGuid() != pCharmedUnit->GetCharmerOrOwnerGuid())
    {
        sLog.outError("HandlePetAction: %s isn't controlled by %s.", petGuid.GetString().c_str(), GetPlayer()->GetGuidStr().c_str());
        return;
    }

    if (!pCharmedUnit->IsAlive())
        return;

    Creature* pCharmedCreature = pCharmedUnit->ToCreature();

    if (!pCharmedCreature)
    {
        // controller player can only do melee attack
        if (!(flag == ACT_REACTION || flag == ACT_COMMAND))
            return;
    }
    else if (pCharmedCreature->IsPet())
    {
        // pet can have action bar disabled
        if (!((Pet*)pCharmedUnit)->IsEnabled())
            return;
    }

    CharmInfo* charmInfo = pCharmedUnit->GetCharmInfo();
    if (!charmInfo)
    {
        sLog.outError("WorldSession::HandlePetAction: object (GUID: %u TypeId: %u) is considered pet-like but doesn't have a charminfo!", pCharmedUnit->GetGUIDLow(), pCharmedUnit->GetTypeId());
        return;
    }

    switch (flag)
    {
        case ACT_COMMAND:                                   // 0x07
            switch (spellid)
            {
                case COMMAND_STAY:                          // flat=1792  //STAY
                    if (!pCharmedUnit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_POSSESSED))
                    {
                        pCharmedUnit->StopMoving();
                        pCharmedUnit->GetMotionMaster()->Clear(false);
                        pCharmedUnit->GetMotionMaster()->MoveIdle();
                        charmInfo->SetIsAtStay(true);
                    }
                    charmInfo->SetCommandState(COMMAND_STAY);

                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsReturning(false);
                    charmInfo->SaveStayPosition();
                    break;
                case COMMAND_FOLLOW:                        // spellid=1792  //FOLLOW
                    if (!pCharmedUnit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_POSSESSED))
                    {
                        pCharmedUnit->AttackStop();
                        pCharmedUnit->InterruptNonMeleeSpells(false);
                        pCharmedUnit->GetMotionMaster()->MoveFollow(_player, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
                    }
                    charmInfo->SetCommandState(COMMAND_FOLLOW);

                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsReturning(true);
                    charmInfo->SetIsCommandFollow(true);
                    charmInfo->SetIsFollowing(false);
                    break;
                case COMMAND_ATTACK:                        // spellid=1792  // ATTACK
                {
                    // Can't attack if owner is pacified
                    if (_player->HasAuraType(SPELL_AURA_MOD_PACIFY))
                    {
                        //pet->SendPetCastFail(spellid, SPELL_FAILED_PACIFIED);
                        /// @todo Send proper error message to client
                        return;
                    }

                    Unit* TargetUnit = _player->GetMap()->GetUnit(targetGuid);
                    if (!TargetUnit)
                        return;

                    if (!GetPlayer()->IsValidAttackTarget(TargetUnit))
                        return;

                    pCharmedUnit->ClearUnitState(UNIT_STAT_FOLLOW);
                    // This is true if pet has no target or has target but targets differs.
                    if (pCharmedUnit->GetVictim() != TargetUnit || (pCharmedUnit->GetVictim() == TargetUnit && !pCharmedUnit->GetCharmInfo()->IsCommandAttack()) || pCharmedUnit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_POSSESSED))
                    {
                        if (pCharmedUnit->GetVictim())
                            pCharmedUnit->AttackStop();

                        if (pCharmedCreature)
                        {
                            charmInfo->SetIsCommandAttack(true);
                            charmInfo->SetIsAtStay(false);
                            charmInfo->SetIsFollowing(false);
                            charmInfo->SetIsCommandFollow(false);
                            charmInfo->SetIsReturning(false);

                            pCharmedUnit->ToCreature()->AI()->AttackStart(TargetUnit);

                            //10% chance to play special pet attack talk, else growl
                            if (pCharmedUnit->ToCreature()->IsPet() && ((Pet*)pCharmedUnit)->getPetType() == SUMMON_PET && pCharmedUnit != TargetUnit && urand(0, 100) < 10)
                                pCharmedUnit->SendPetTalk((uint32)PET_TALK_ATTACK);
                            else
                            {
                                // 90% chance for pet and 100% chance for charmed creature
                                pCharmedUnit->SendPetAIReaction();
                            }
                        }
                        else                                // charmed player
                        {
                            if (pCharmedUnit->GetVictim() && pCharmedUnit->GetVictim() != TargetUnit)
                                pCharmedUnit->AttackStop();

                            charmInfo->SetIsCommandAttack(true);
                            charmInfo->SetIsAtStay(false);
                            charmInfo->SetIsFollowing(false);
                            charmInfo->SetIsCommandFollow(false);
                            charmInfo->SetIsReturning(false);

                            pCharmedUnit->Attack(TargetUnit, true);
                            pCharmedUnit->SendPetAIReaction();
                            pCharmedUnit->GetMotionMaster()->MoveChase(TargetUnit);
                        }
                    }
                    break;
                }
                case COMMAND_DISMISS:                       // pet dismiss (summoned pet)
                {
                    if (Pet* pPet = pCharmedUnit->ToPet())
                    {
                        // Hunter pets are dismissed with a spell with a cast time
                        if (pPet->getPetType() != HUNTER_PET)
                            // dismissing a summoned pet is like killing them (this prevents returning a soulshard...)
                            pPet->Unsummon(PET_SAVE_NOT_IN_SLOT);
                    }
                    else                                    // charmed
                        _player->Uncharm();
                    break;
                }  
                default:
                    sLog.outError("WORLD: unknown PET flag Action %i and spellid %i.", uint32(flag), spellid);
            }
            break;
        case ACT_REACTION:                                  // 0x6
            switch (spellid)
            {
                case REACT_PASSIVE:                         //passive
                    pCharmedUnit->AttackStop();
                // no break
                case REACT_DEFENSIVE:                       //recovery
                case REACT_AGGRESSIVE:                      //activete
                    if (pCharmedCreature)
                        pCharmedCreature->SetReactState(ReactStates(spellid));
                    break;
            }
            break;
        case ACT_DISABLED:                                  // 0x81    spell (disabled), ignore
        case ACT_PASSIVE:                                   // 0x01
        case ACT_ENABLED:                                   // 0xC1    spell
        {
            Unit* unit_target = nullptr;
            if (targetGuid)
                unit_target = _player->GetMap()->GetUnit(targetGuid);

            // do not cast unknown spells
            SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellid);
            if (!spellInfo)
            {
                sLog.outError("WORLD: unknown PET spell id %i", spellid);
                return;
            }

            if (pCharmedUnit->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo))
                return;

            for (uint32 i : spellInfo->EffectImplicitTargetA)
            {
                if (i == TARGET_ALL_ENEMY_IN_AREA || i == TARGET_ALL_ENEMY_IN_AREA_INSTANT || i == TARGET_ALL_ENEMY_IN_AREA_CHANNELED)
                    return;
            }

            // do not cast not learned spells
            if (!pCharmedUnit->HasSpell(spellid) || spellInfo->IsPassiveSpell())
                return;

            pCharmedUnit->ClearUnitState(UNIT_STAT_MOVING);

            Spell* spell = new Spell(pCharmedUnit, spellInfo, false);

            SpellCastResult result = spell->CheckPetCast(unit_target);

            // auto turn to target unless possessed
            if (result == SPELL_FAILED_UNIT_NOT_INFRONT && !pCharmedUnit->HasAuraType(SPELL_AURA_MOD_POSSESS))
            {
                if (unit_target)
                {
                    pCharmedUnit->SetInFront(unit_target);
                    if (unit_target->GetTypeId() == TYPEID_PLAYER)
                        pCharmedUnit->SendCreateUpdateToPlayer((Player*)unit_target);
                }
                else if (Unit* unit_target2 = spell->m_targets.getUnitTarget())
                {
                    pCharmedUnit->SetInFront(unit_target2);
                    if (unit_target2->GetTypeId() == TYPEID_PLAYER)
                        pCharmedUnit->SendCreateUpdateToPlayer((Player*)unit_target2);
                }
                if (Unit* powner = pCharmedUnit->GetCharmerOrOwner())
                    if (powner->GetTypeId() == TYPEID_PLAYER)
                        pCharmedUnit->SendCreateUpdateToPlayer((Player*)powner);
                result = SPELL_CAST_OK;
            }

            if (result == SPELL_CAST_OK)
            {
                if (((Creature*)pCharmedUnit)->IsPet())
                    ((Pet*)pCharmedUnit)->CheckLearning(spellid);

                unit_target = spell->m_targets.getUnitTarget();

                // 10% chance to play special pet attack talk, else growl
                // actually this only seems to happen on special spells, fire shield for imp, torment for voidwalker, but it's stupid to check every spell
                if (((Creature*)pCharmedUnit)->IsPet() && (((Pet*)pCharmedUnit)->getPetType() == SUMMON_PET) && (pCharmedUnit != unit_target) && (urand(0, 100) < 10))
                    pCharmedUnit->SendPetTalk((uint32)PET_TALK_SPECIAL_SPELL);
                else
                    pCharmedUnit->SendPetAIReaction();

                if (unit_target && !GetPlayer()->IsFriendlyTo(unit_target) && !pCharmedUnit->HasAuraType(SPELL_AURA_MOD_POSSESS))
                {
                    // This is true if pet has no target or has target but targets differs.
                    if (pCharmedUnit->GetVictim() != unit_target)
                    {
                        if (pCharmedUnit->GetVictim())
                            pCharmedUnit->AttackStop();
                        pCharmedUnit->GetMotionMaster()->Clear();
                        if (((Creature*)pCharmedUnit)->AI())
                            ((Creature*)pCharmedUnit)->AI()->AttackStart(unit_target);
                    }
                }

                spell->prepare();
            }
            else
            {
                if (pCharmedUnit->HasAuraType(SPELL_AURA_MOD_POSSESS))
                    Spell::SendCastResult(GetPlayer(), spellInfo, result);
                else
                    pCharmedUnit->SendPetCastFail(spellid, result);

                if (!((Creature*)pCharmedUnit)->HasSpellCooldown(spellid))
                    GetPlayer()->SendClearCooldown(spellid, pCharmedUnit);

                spell->finish(false);
                spell->Delete();
            }
            break;
        }
        default:
            sLog.outError("WORLD: unknown PET flag Action %i and spellid %i.", uint32(flag), spellid);
    }
}

void WorldSession::HandlePetStopAttack(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received CMSG_PET_STOP_ATTACK");

    ObjectGuid petGuid;
    recv_data >> petGuid;

    Unit* pet = GetPlayer()->GetMap()->GetUnit(petGuid);    // pet or controlled creature/player
    if (!pet)
    {
        sLog.outError("%s doesn't exist.", petGuid.GetString().c_str());
        return;
    }

    if (GetPlayer()->GetObjectGuid() != pet->GetCharmerOrOwnerGuid())
    {
        sLog.outError("HandlePetStopAttack: %s isn't charm/pet of %s.", petGuid.GetString().c_str(), GetPlayer()->GetGuidStr().c_str());
        return;
    }

    if (!pet->IsAlive())
        return;

    pet->AttackStop();
}

void WorldSession::HandlePetNameQueryOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("HandlePetNameQuery. CMSG_PET_NAME_QUERY");

    uint32 petnumber;
    ObjectGuid petguid;

    recv_data >> petnumber;
    recv_data >> petguid;

    SendPetNameQuery(petguid, petnumber);
}

void WorldSession::SendPetNameQuery(ObjectGuid petguid, uint32 petnumber)
{
    Creature* pet = _player->GetMap()->GetAnyTypeCreature(petguid);
    if (!pet || !pet->GetCharmInfo() || pet->GetCharmInfo()->GetPetNumber() != petnumber)
        return;

    std::string name = pet->GetName();

    WorldPacket data(SMSG_PET_NAME_QUERY_RESPONSE, (4 + 4 + name.size() + 1));
    data << uint32(petnumber);
    data << name.c_str();
    data << uint32(pet->GetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP));

    _player->GetSession()->SendPacket(&data);
}

void WorldSession::HandlePetSetAction(WorldPacket& recv_data)
{
    DETAIL_LOG("HandlePetSetAction. CMSG_PET_SET_ACTION");

    ObjectGuid petGuid;
    uint8  count;

    recv_data >> petGuid;

    Creature* pet = _player->GetMap()->GetAnyTypeCreature(petGuid);

    if (!pet || (pet != _player->GetPet() && pet != _player->GetCharm()))
    {
        sLog.outError("HandlePetSetAction: Unknown pet or pet owner.");
        return;
    }

    // pet can have action bar disabled
    if (pet->IsPet() && !((Pet*)pet)->IsEnabled())
        return;

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
    {
        sLog.outError("WorldSession::HandlePetSetAction: object (GUID: %u TypeId: %u) is considered pet-like but doesn't have a charminfo!", pet->GetGUIDLow(), pet->GetTypeId());
        return;
    }

    count = (recv_data.size() == 24) ? 2 : 1;

    uint32 position[2];
    uint32 data[2];
    bool move_command = false;

    for (uint8 i = 0; i < count; ++i)
    {
        recv_data >> position[i];
        recv_data >> data[i];

        uint8 act_state = UNIT_ACTION_BUTTON_TYPE(data[i]);

        // ignore invalid position
        if (position[i] >= MAX_UNIT_ACTION_BAR_INDEX)
            return;

        // in the normal case, command and reaction buttons can only be moved, not removed
        // at moving count ==2, at removing count == 1
        // ignore attempt to remove command|reaction buttons (not possible at normal case)
        if (act_state == ACT_COMMAND || act_state == ACT_REACTION)
        {
            if (count == 1)
                return;

            move_command = true;
        }
    }

    // check swap (at command->spell swap client remove spell first in another packet, so check only command move correctness)
    if (move_command)
    {
        uint8 act_state_0 = UNIT_ACTION_BUTTON_TYPE(data[0]);
        if (act_state_0 == ACT_COMMAND || act_state_0 == ACT_REACTION)
        {
            uint32 spell_id_0 = UNIT_ACTION_BUTTON_ACTION(data[0]);
            UnitActionBarEntry const* actionEntry_1 = charmInfo->GetActionBarEntry(position[1]);
            if (!actionEntry_1 || spell_id_0 != actionEntry_1->GetAction() ||
                    act_state_0 != actionEntry_1->GetType())
                return;
        }

        uint8 act_state_1 = UNIT_ACTION_BUTTON_TYPE(data[1]);
        if (act_state_1 == ACT_COMMAND || act_state_1 == ACT_REACTION)
        {
            uint32 spell_id_1 = UNIT_ACTION_BUTTON_ACTION(data[1]);
            UnitActionBarEntry const* actionEntry_0 = charmInfo->GetActionBarEntry(position[0]);
            if (!actionEntry_0 || spell_id_1 != actionEntry_0->GetAction() ||
                    act_state_1 != actionEntry_0->GetType())
                return;
        }
    }

    for (uint8 i = 0; i < count; ++i)
    {
        uint32 spell_id = UNIT_ACTION_BUTTON_ACTION(data[i]);
        uint8 act_state = UNIT_ACTION_BUTTON_TYPE(data[i]);

        DETAIL_LOG("Player %s has changed pet spell action. Position: %u, Spell: %u, State: 0x%X", _player->GetName(), position[i], spell_id, uint32(act_state));

        // if it's act for spell (en/disable/cast) and there is a spell given (0 = remove spell) which pet doesn't know, don't add
        if (!((act_state == ACT_ENABLED || act_state == ACT_DISABLED || act_state == ACT_PASSIVE) && spell_id && !pet->HasSpell(spell_id)))
        {
            // sign for autocast
            if (act_state == ACT_ENABLED && spell_id)
            {
                if (pet->IsCharmed())
                    charmInfo->ToggleCreatureAutocast(spell_id, true);
                else
                    ((Pet*)pet)->ToggleAutocast(spell_id, true);
            }
            // sign for no/turn off autocast
            else if (act_state == ACT_DISABLED && spell_id)
            {
                if (pet->IsCharmed())
                    charmInfo->ToggleCreatureAutocast(spell_id, false);
                else
                    ((Pet*)pet)->ToggleAutocast(spell_id, false);
            }

            charmInfo->SetActionBar(position[i], spell_id, ActiveStates(act_state));
        }
    }
}

void WorldSession::HandlePetRename(WorldPacket& recv_data)
{
    DETAIL_LOG("HandlePetRename. CMSG_PET_RENAME");

    ObjectGuid petGuid;
    std::string name;

    recv_data >> petGuid;
    recv_data >> name;

    Pet* pet = _player->GetMap()->GetPet(petGuid);
    // check it!
    if (!pet || pet->getPetType() != HUNTER_PET ||
            !pet->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_RENAME) ||
            pet->GetOwnerGuid() != _player->GetObjectGuid() || !pet->GetCharmInfo())
        return;

    PetNameInvalidReason res = ObjectMgr::CheckPetName(name);
    if (res != PET_NAME_SUCCESS)
    {
        SendPetNameInvalid(res, name);
        return;
    }

    if (sObjectMgr.IsReservedName(name))
    {
        SendPetNameInvalid(PET_NAME_RESERVED, name);
        return;
    }

    pet->SetName(name);

    if (_player->GetGroup())
        _player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_NAME);

    pet->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_RENAME);

    CharacterDatabase.BeginTransaction();
    CharacterDatabase.escape_string(name);
    CharacterDatabase.PExecute("UPDATE character_pet SET name = '%s', renamed = '1' WHERE owner = '%u' AND id = '%u'", name.c_str(), _player->GetGUIDLow(), pet->GetCharmInfo()->GetPetNumber());
    CharacterDatabase.CommitTransaction();

    pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(time(nullptr)));
}

void WorldSession::HandlePetAbandon(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;                                      // pet guid

    DETAIL_LOG("HandlePetAbandon. CMSG_PET_ABANDON pet guid is %s", guid.GetString().c_str());

    if (!_player->IsInWorld())
        return;

    // pet/charmed
    if (Unit* petUnit = _player->GetMap()->GetUnit(guid))
    {
        if (petUnit->GetOwnerGuid() != _player->GetObjectGuid() || !petUnit->GetCharmInfo())
            return;

        Creature* petCreature = petUnit->GetTypeId() == TYPEID_UNIT ? static_cast<Creature*>(petUnit) : nullptr;
        Pet* pet = petCreature && petCreature->IsPet() ? static_cast<Pet*>(petUnit) : nullptr;

        if (pet)
        {
            // Permanently abandon pet
            if (pet->getPetType() == HUNTER_PET)
                pet->Unsummon(PET_SAVE_AS_DELETED, _player);
            // Simply dismiss
            else
                pet->Unsummon(PET_SAVE_NOT_IN_SLOT);
        }
        else if (petUnit->GetObjectGuid() == _player->GetCharmGuid())
        {
            _player->Uncharm();
        }
    }
}

void WorldSession::HandlePetUnlearnOpcode(WorldPacket& recvPacket)
{
    DETAIL_LOG("CMSG_PET_UNLEARN");

    ObjectGuid guid;
    recvPacket >> guid;                 // Pet guid

    Pet* pet = _player->GetPet();

    if (!pet || guid != pet->GetObjectGuid())
    {
        sLog.outError("HandlePetUnlearnOpcode. %s isn't pet of %s .", guid.GetString().c_str(), GetPlayer()->GetGuidStr().c_str());
        return;
    }

    if (pet->getPetType() != HUNTER_PET || pet->m_petSpells.size() <= 1)
        return;

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
    {
        sLog.outError("WorldSession::HandlePetUnlearnOpcode: %s is considered pet-like but doesn't have a charminfo!", pet->GetGuidStr().c_str());
        return;
    }

    uint32 cost = pet->GetResetTalentsCost();

    if (GetPlayer()->GetMoney() < cost)
    {
        GetPlayer()->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, 0, 0, 0);
        return;
    }

    for (PetSpellMap::iterator itr = pet->m_petSpells.begin(); itr != pet->m_petSpells.end();)
    {
        uint32 spell_id = itr->first;                       // Pet::RemoveSpell can invalidate iterator at erase NEW spell
        ++itr;
        pet->unlearnSpell(spell_id, false);
    }

    pet->SetTP(pet->GetLevel() * (pet->GetLoyaltyLevel() - 1));

    for (int i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
        if (UnitActionBarEntry const* ab = charmInfo->GetActionBarEntry(i))
            if (ab->GetAction() && ab->IsActionBarForSpell())
                charmInfo->SetActionBar(i, 0, ACT_DISABLED);

    // relearn pet passives
    pet->LearnPetPassives();

    pet->m_resetTalentsTime = time(nullptr);
    pet->m_resetTalentsCost = cost;
    GetPlayer()->ModifyMoney(-(int32)cost);

    GetPlayer()->PetSpellInitialize();
}

void WorldSession::HandlePetSpellAutocastOpcode(WorldPacket& recvPacket)
{
    DETAIL_LOG("CMSG_PET_SPELL_AUTOCAST");

    ObjectGuid guid;
    uint32 spellid;
    uint8  state;                                           // 1 for on, 0 for off
    recvPacket >> guid >> spellid >> state;

    Creature* pet = _player->GetMap()->GetAnyTypeCreature(guid);
    if (!pet || (guid != _player->GetPetGuid() && guid != _player->GetCharmGuid()))
    {
        sLog.outError("HandlePetSpellAutocastOpcode. %s isn't pet of %s .", guid.GetString().c_str(), GetPlayer()->GetGuidStr().c_str());
        return;
    }

    // do not add not learned spells/ passive spells
    if (!pet->HasSpell(spellid) || Spells::IsPassiveSpell(spellid))
        return;

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
    {
        sLog.outError("WorldSession::HandlePetSpellAutocastOpcod: %s is considered pet-like but doesn't have a charminfo!", guid.GetString().c_str());
        return;
    }

    if (pet->IsCharmed())
        // state can be used as boolean
        pet->GetCharmInfo()->ToggleCreatureAutocast(spellid, state);
    else
        ((Pet*)pet)->ToggleAutocast(spellid, state);

    charmInfo->SetSpellAutocast(spellid, state);
}

void WorldSession::HandlePetCastSpellOpcode(WorldPacket& recvPacket)
{
    DETAIL_LOG("WORLD: CMSG_PET_CAST_SPELL");

    ObjectGuid guid;
    uint32 spellid;

    recvPacket >> guid >> spellid;

    DEBUG_LOG("WORLD: CMSG_PET_CAST_SPELL, %s, spellid %u", guid.GetString().c_str(), spellid);

    Creature* pet = _player->GetMap()->GetAnyTypeCreature(guid);

    if (!pet || (guid != _player->GetPetGuid() && guid != _player->GetCharmGuid()))
    {
        sLog.outError("HandlePetCastSpellOpcode: %s isn't pet of %s .", guid.GetString().c_str(), GetPlayer()->GetGuidStr().c_str());
        return;
    }

    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellid);
    if (!spellInfo)
    {
        sLog.outError("WORLD: unknown PET spell id %i", spellid);
        return;
    }

    if (pet->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo))
        return;

    // do not cast not learned spells
    if (!pet->HasSpell(spellid) || spellInfo->IsPassiveSpell())
        return;

    SpellCastTargets targets;

    recvPacket >> targets.ReadForCaster(pet);

    pet->ClearUnitState(UNIT_STAT_MOVING);

    Spell* spell = new Spell(pet, spellInfo, false);
    spell->m_targets = targets;

    SpellCastResult result = spell->CheckPetCast(nullptr);
    if (result == SPELL_CAST_OK)
    {
        if (pet->IsPet())
        {
            ((Pet*)pet)->CheckLearning(spellid);

            //10% chance to play special pet attack talk, else growl
            //actually this only seems to happen on special spells, fire shield for imp, torment for voidwalker, but it's stupid to check every spell
            if (((Pet*)pet)->getPetType() == SUMMON_PET && (urand(0, 100) < 10))
                pet->SendPetTalk((uint32)PET_TALK_SPECIAL_SPELL);
            else
                pet->SendPetAIReaction();
        }

        spell->prepare();
    }
    else
    {
        pet->SendPetCastFail(spellid, result);
        if (!pet->HasSpellCooldown(spellid))
            GetPlayer()->SendClearCooldown(spellid, pet);

        spell->finish(false);
        spell->Delete();
    }
}

void WorldSession::SendPetNameInvalid(uint32 error, std::string const& name)
{
    WorldPacket data(SMSG_PET_NAME_INVALID, 0);
    SendPacket(&data);
}
