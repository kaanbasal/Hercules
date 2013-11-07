// Copyright (c) Hercules Dev Team, licensed under GNU GPL.
// See the LICENSE file
// Portions Copyright (c) Athena Dev Teams

#include "../common/cbasetypes.h"
#include "../common/timer.h"
#include "../common/socket.h" // last_tick
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/random.h"
#include "../common/showmsg.h"
#include "../common/utils.h"
#include "../common/strlib.h"

#include "party.h"
#include "atcommand.h"	//msg_txt()
#include "pc.h"
#include "map.h"
#include "instance.h"
#include "battle.h"
#include "intif.h"
#include "clif.h"
#include "log.h"
#include "skill.h"
#include "status.h"
#include "itemdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


struct party_interface party_s;

/*==========================================
 * Fills the given party_member structure according to the sd provided.
 * Used when creating/adding people to a party. [Skotlex]
 *------------------------------------------*/
void party_fill_member(struct party_member* member, struct map_session_data* sd, unsigned int leader) {
  	member->account_id = sd->status.account_id;
	member->char_id    = sd->status.char_id;
	safestrncpy(member->name, sd->status.name, NAME_LENGTH);
	member->class_     = sd->status.class_;
	member->map        = sd->mapindex;
	member->lv         = sd->status.base_level;
	member->online     = 1;
	member->leader     = leader;
}
/// Get the member_id of a party member.
/// Return -1 if not in party.
int party_getmemberid(struct party_data* p, struct map_session_data* sd) {
	int member_id;
	nullpo_retr(-1, p);
	if( sd == NULL )
		return -1;// no player
	ARR_FIND(0, MAX_PARTY, member_id,
		p->party.member[member_id].account_id == sd->status.account_id &&
		p->party.member[member_id].char_id == sd->status.char_id);
	if( member_id == MAX_PARTY )
		return -1;// not found
	return member_id;
}

/*==========================================
 * Request an available sd of this party
 *------------------------------------------*/
struct map_session_data* party_getavailablesd(struct party_data *p)
{
	int i;
	nullpo_retr(NULL, p);
	ARR_FIND(0, MAX_PARTY, i, p->data[i].sd != NULL);
	return( i < MAX_PARTY ) ? p->data[i].sd : NULL;
}

/*==========================================
 * Retrieves and validates the sd pointer for this party member [Skotlex]
 *------------------------------------------*/
TBL_PC* party_sd_check(int party_id, int account_id, int char_id) {
	TBL_PC* sd = map->id2sd(account_id);

	if (!(sd && sd->status.char_id == char_id))
		return NULL;

	if( sd->status.party_id == 0 )
		sd->status.party_id = party_id;// auto-join if not in a party
	if (sd->status.party_id != party_id)
	{	//If player belongs to a different party, kick him out.
		intif->party_leave(party_id,account_id,char_id);
		return NULL;
	}

	return sd;
}
int party_db_final(DBKey key, DBData *data, va_list ap) {
	struct party_data *p;
	
	if( ( p = DB->data2ptr(data) ) && p->instance )
		aFree(p->instance);
	
	return 0;
}
/// Party data lookup using party id.
struct party_data* party_search(int party_id)
{
	if(!party_id)
		return NULL;
	return (struct party_data*)idb_get(party->db,party_id);
}

/// Party data lookup using party name.
struct party_data* party_searchname(const char* str)
{
	struct party_data* p;

	DBIterator *iter = db_iterator(party->db);
	for( p = dbi_first(iter); dbi_exists(iter); p = dbi_next(iter) )
	{
		if( strncmpi(p->party.name,str,NAME_LENGTH) == 0 )
			break;
	}
	dbi_destroy(iter);

	return p;
}

int party_create(struct map_session_data *sd,char *name,int item,int item2)
{
	struct party_member leader;
	char tname[NAME_LENGTH];

	safestrncpy(tname, name, NAME_LENGTH);
	trim(tname);

	if( !tname[0] )
	{// empty name
		return 0;
	}

	if( sd->status.party_id > 0 || sd->party_joining || sd->party_creating )
	{// already associated with a party
		clif->party_created(sd,2);
		return 0;
	}

	sd->party_creating = true;

	party->fill_member(&leader, sd, 1);

	intif->create_party(&leader,name,item,item2);
	return 0;
}


void party_created(int account_id,int char_id,int fail,int party_id,char *name) {
	struct map_session_data *sd;
	sd=map->id2sd(account_id);

	if (!sd || sd->status.char_id != char_id || !sd->party_creating ) {
		//Character logged off before creation ack?
		if (!fail) //break up party since player could not be added to it.
			intif->party_leave(party_id,account_id,char_id);
		return;
	}

	sd->party_creating = false;

	if( !fail ) {
		sd->status.party_id = party_id;
		clif->party_created(sd,0); //Success message
		//We don't do any further work here because the char-server sends a party info packet right after creating the party.
	} else {
		clif->party_created(sd,1); // "party name already exists"
	}

}

int party_request_info(int party_id, int char_id)
{
	return intif->request_partyinfo(party_id, char_id);
}

/// Invoked (from char-server) when the party info is not found.
int party_recv_noinfo(int party_id, int char_id) {
	party->broken(party_id);
	if( char_id != 0 ) {
		// requester
		struct map_session_data* sd;
		sd = map->charid2sd(char_id);
		if( sd && sd->status.party_id == party_id )
			sd->status.party_id = 0;
	}
	return 0;
}

void party_check_state(struct party_data *p) {
	int i;
	memset(&p->state, 0, sizeof(p->state));
	for (i = 0; i < MAX_PARTY; i ++) {
		if (!p->party.member[i].online) continue; //Those not online shouldn't aport to skill usage and all that.
		switch (p->party.member[i].class_) {
			case JOB_MONK:
			case JOB_BABY_MONK:
			case JOB_CHAMPION:
				p->state.monk = 1;
			break;
			case JOB_STAR_GLADIATOR:
				p->state.sg = 1;
			break;
			case JOB_SUPER_NOVICE:
			case JOB_SUPER_BABY:
				p->state.snovice = 1;
			break;
			case JOB_TAEKWON:
				p->state.tk = 1;
			break;
		}
	}
}

int party_recv_info(struct party* sp, int char_id)
{
	struct party_data* p;
	struct party_member* member;
	struct map_session_data* sd;
	int removed[MAX_PARTY];// member_id in old data
	int removed_count = 0;
	int added[MAX_PARTY];// member_id in new data
	int added_count = 0;
	int i,j;
	int member_id;

	nullpo_ret(sp);

	p = (struct party_data*)idb_get(party->db, sp->party_id);
	if( p != NULL ) {// diff members
		for( member_id = 0; member_id < MAX_PARTY; ++member_id ) {
			member = &p->party.member[member_id];
			if( member->char_id == 0 )
				continue;// empty
			ARR_FIND(0, MAX_PARTY, i,
				sp->member[i].account_id == member->account_id &&
				sp->member[i].char_id == member->char_id);
			if( i == MAX_PARTY )
				removed[removed_count++] = member_id;
		}
		for( member_id = 0; member_id < MAX_PARTY; ++member_id ) {
			member = &sp->member[member_id];
			if( member->char_id == 0 )
				continue;// empty
			ARR_FIND(0, MAX_PARTY, i,
				p->party.member[i].account_id == member->account_id &&
				p->party.member[i].char_id == member->char_id);
			if( i == MAX_PARTY )
				added[added_count++] = member_id;
		}
	} else {
		for( member_id = 0; member_id < MAX_PARTY; ++member_id )
			if( sp->member[member_id].char_id != 0 )
				added[added_count++] = member_id;
		CREATE(p, struct party_data, 1);
		p->instance = NULL;
		p->instances = 0;
		idb_put(party->db, sp->party_id, p);
	}
	while( removed_count > 0 ) {// no longer in party
		member_id = removed[--removed_count];
		sd = p->data[member_id].sd;
		if( sd == NULL )
			continue;// not online
		party->member_withdraw(sp->party_id, sd->status.account_id, sd->status.char_id);
	}
	memcpy(&p->party, sp, sizeof(struct party));
	memset(&p->state, 0, sizeof(p->state));
	memset(&p->data, 0, sizeof(p->data));
	for( member_id = 0; member_id < MAX_PARTY; member_id++ ) {
		member = &p->party.member[member_id];
		if ( member->char_id == 0 )
			continue;// empty
		p->data[member_id].sd = party->sd_check(sp->party_id, member->account_id, member->char_id);
	}
	party->check_state(p);
	while( added_count > 0 ) { // new in party
		member_id = added[--added_count];
		sd = p->data[member_id].sd;
		if( sd == NULL )
			continue;// not online
		clif->charnameupdate(sd); //Update other people's display. [Skotlex]
		clif->party_member_info(p,sd);
		clif->party_option(p,sd,0x100);
		clif->party_info(p,NULL);
		for( j = 0; j < p->instances; j++ ) {
			if( p->instance[j] >= 0 ) {
				if( instance->list[p->instance[j]].idle_timer == INVALID_TIMER && instance->list[p->instance[j]].progress_timer == INVALID_TIMER )
					continue;
				clif->instance_join(sd->fd, p->instance[j]);
				break;
			}
		}
	}
	if( char_id != 0 ) {
		// requester
		sd = map->charid2sd(char_id);
		if( sd && sd->status.party_id == sp->party_id && party->getmemberid(p,sd) == -1 )
			sd->status.party_id = 0;// was not in the party
	}
	return 0;
}

int party_invite(struct map_session_data *sd,struct map_session_data *tsd)
{
	struct party_data *p;
	int i;

	nullpo_ret(sd);

	if( ( p = party->search(sd->status.party_id) ) == NULL )
		return 0;

	// confirm if this player is a party leader
	ARR_FIND(0, MAX_PARTY, i, p->data[i].sd == sd);

	if( i == MAX_PARTY || !p->party.member[i].leader ) {
		clif->message(sd->fd, msg_txt(282));
		return 0;
	}

	// confirm if there is an open slot in the party
	ARR_FIND(0, MAX_PARTY, i, p->party.member[i].account_id == 0);

	if( i == MAX_PARTY ) {
		clif->party_inviteack(sd, (tsd?tsd->status.name:""), 3);
		return 0;
	}

	// confirm whether the account has the ability to invite before checking the player
	if( !pc->has_permission(sd, PC_PERM_PARTY) || (tsd && !pc->has_permission(tsd, PC_PERM_PARTY)) ) {
		clif->message(sd->fd, msg_txt(81)); // "Your GM level doesn't authorize you to preform this action on the specified player."
		return 0;
	}

	if( tsd == NULL) {
		clif->party_inviteack(sd, "", 7);
		return 0;
	}

	if(!battle_config.invite_request_check) {
		if (tsd->guild_invite>0 || tsd->trade_partner || tsd->adopt_invite) {
			clif->party_inviteack(sd,tsd->status.name,0);
			return 0;
		}
	}

	if (!tsd->fd) { //You can't invite someone who has already disconnected.
		clif->party_inviteack(sd,tsd->status.name,1);
		return 0;
	}

	if( tsd->status.party_id > 0 || tsd->party_invite > 0 )
	{// already associated with a party
		clif->party_inviteack(sd,tsd->status.name,0);
		return 0;
	}

	tsd->party_invite=sd->status.party_id;
	tsd->party_invite_account=sd->status.account_id;

	clif->party_invite(sd,tsd);
	return 1;
}

void party_reply_invite(struct map_session_data *sd,int party_id,int flag) {
	struct map_session_data* tsd;
	struct party_member member;

	if( sd->party_invite != party_id )
	{// forged
		sd->party_invite = 0;
		sd->party_invite_account = 0;
		return;
	}
	tsd = map->id2sd(sd->party_invite_account);

	if( flag == 1 && !sd->party_creating && !sd->party_joining )
	{// accepted and allowed
		sd->party_joining = true;
		party->fill_member(&member, sd, 0);
		intif->party_addmember(sd->party_invite, &member);
	}
	else
	{// rejected or failure
		sd->party_invite = 0;
		sd->party_invite_account = 0;
		if( tsd != NULL )
			clif->party_inviteack(tsd,sd->status.name,1);
	}
}

//Invoked when a player joins:
//- Loads up party data if not in server
//- Sets up the pointer to him
//- Player must be authed/active and belong to a party before calling this method
void party_member_joined(struct map_session_data *sd)
{
	struct party_data* p = party->search(sd->status.party_id);
	int i;
	if (!p) {
		party->request_info(sd->status.party_id, sd->status.char_id);
		return;
	}
	ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == sd->status.account_id && p->party.member[i].char_id == sd->status.char_id );
	if (i < MAX_PARTY) {
		int j;
		p->data[i].sd = sd;
		for( j = 0; j < p->instances; j++ ) {
			if( p->instance[j] >= 0 ) {
				if( instance->list[p->instance[j]].idle_timer == INVALID_TIMER && instance->list[p->instance[j]].progress_timer == INVALID_TIMER )
					continue;
				clif->instance_join(sd->fd, p->instance[j]);
				break;
			}
		}
	} else
		sd->status.party_id = 0; //He does not belongs to the party really?
}

/// Invoked (from char-server) when a new member is added to the party.
/// flag: 0-success, 1-failure
int party_member_added(int party_id,int account_id,int char_id, int flag) {
	struct map_session_data *sd = map->id2sd(account_id),*sd2;
	struct party_data *p = party->search(party_id);
	int i, j;

	if(sd == NULL || sd->status.char_id != char_id || !sd->party_joining ) {
		if (!flag) //Char logged off before being accepted into party.
			intif->party_leave(party_id,account_id,char_id);
		return 0;
	}

	sd2 = map->id2sd(sd->party_invite_account);

	sd->party_joining = false;
	sd->party_invite = 0;
	sd->party_invite_account = 0;

	if (!p) {
		ShowError("party_member_added: party %d not found.\n",party_id);
		intif->party_leave(party_id,account_id,char_id);
		return 0;
	}

	if( flag )
	{// failed
		if( sd2 != NULL )
			clif->party_inviteack(sd2,sd->status.name,3);
		return 0;
	}

	sd->status.party_id = party_id;

	clif->party_member_info(p,sd);
	clif->party_info(p,sd);

	if( sd2 != NULL )
		clif->party_inviteack(sd2,sd->status.name,2);

	for( i = 0; i < ARRAYLENGTH(p->data); ++i )
	{// hp of the other party members
		sd2 = p->data[i].sd;
		if( sd2 && sd2->status.account_id != account_id && sd2->status.char_id != char_id )
			clif->hpmeter_single(sd->fd, sd2->bl.id, sd2->battle_status.hp, sd2->battle_status.max_hp);
	}
	clif->party_hp(sd);
	clif->party_xy(sd);
	clif->charnameupdate(sd); //Update char name's display [Skotlex]

	for( j = 0; j < p->instances; j++ ) {
		if( p->instance[j] >= 0 ) {
			if( instance->list[p->instance[j]].idle_timer == INVALID_TIMER && instance->list[p->instance[j]].progress_timer == INVALID_TIMER )
				continue;
			clif->instance_join(sd->fd, p->instance[j]);
			break;
		}
	}
	
	return 0;
}

/// Party member 'sd' requesting kick of member with <account_id, name>.
int party_removemember(struct map_session_data* sd, int account_id, char* name)
{
	struct party_data *p;
	int i;

	p = party->search(sd->status.party_id);
	if( p == NULL )
		return 0;

	// check the requesting char's party membership
	ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == sd->status.account_id && p->party.member[i].char_id == sd->status.char_id );
	if( i == MAX_PARTY )
		return 0; // request from someone not in party? o.O
	if( !p->party.member[i].leader )
		return 0; // only party leader may remove members

	ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == account_id && strncmp(p->party.member[i].name,name,NAME_LENGTH) == 0 );
	if( i == MAX_PARTY )
		return 0; // no such char in party

	intif->party_leave(p->party.party_id,account_id,p->party.member[i].char_id);
	return 1;
}

/// Party member 'sd' requesting exit from party.
int party_leave(struct map_session_data *sd)
{
	struct party_data *p;
	int i;

	p = party->search(sd->status.party_id);
	if( p == NULL )
		return 0;

	ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == sd->status.account_id && p->party.member[i].char_id == sd->status.char_id );
	if( i == MAX_PARTY )
		return 0;

	intif->party_leave(p->party.party_id,sd->status.account_id,sd->status.char_id);
	return 1;
}

/// Invoked (from char-server) when a party member leaves the party.
int party_member_withdraw(int party_id, int account_id, int char_id)
{
	struct map_session_data* sd = map->id2sd(account_id);
	struct party_data* p = party->search(party_id);

	if( p ) {
		int i;
		ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == account_id && p->party.member[i].char_id == char_id );
		if( i < MAX_PARTY ) {
			clif->party_withdraw(p,sd,account_id,p->party.member[i].name,0x0);
			memset(&p->party.member[i], 0, sizeof(p->party.member[0]));
			memset(&p->data[i], 0, sizeof(p->data[0]));
			p->party.count--;
			party->check_state(p);
		}
	}

	if( sd && sd->status.party_id == party_id && sd->status.char_id == char_id ) {
#ifdef BOUND_ITEMS
		int idxlist[MAX_INVENTORY]; //or malloc to reduce consumtion
		int j,i;
		j = pc->bound_chk(sd,3,idxlist);
		for(i=0;i<j;i++)
			pc->delitem(sd,idxlist[i],sd->status.inventory[idxlist[i]].amount,0,1,LOG_TYPE_OTHER);
#endif
	sd->status.party_id = 0;
		clif->charnameupdate(sd); //Update name display [Skotlex]
		//TODO: hp bars should be cleared too
		if( p->instances )
			instance->check_kick(sd);
	}
	if (sd && sd->sc.data[SC_DANCING]) {
		status_change_end(&sd->bl, SC_DANCING, INVALID_TIMER);
		status_change_end(&sd->bl, SC_DRUMBATTLE, INVALID_TIMER);
		status_change_end(&sd->bl, SC_NIBELUNGEN, INVALID_TIMER);
		status_change_end(&sd->bl, SC_SIEGFRIED, INVALID_TIMER);
	}
	return 0;
}

/// Invoked (from char-server) when a party is disbanded.
int party_broken(int party_id)
{
	struct party_data* p;
	int i, j;

	p = party->search(party_id);
	if( p == NULL )
		return 0;

	for( j = 0; j < p->instances; j++ ) {
		if( p->instance[j] >= 0 ) {
			instance->destroy( p->instance[j] );
			instance->list[p->instance[j]].owner_id = 0;
		}
	}
	
	for( i = 0; i < MAX_PARTY; i++ ) {
		if( p->data[i].sd!=NULL ) {
			clif->party_withdraw(p,p->data[i].sd,p->party.member[i].account_id,p->party.member[i].name,0x10);
			p->data[i].sd->status.party_id=0;
		}
	}

	idb_remove(party->db,party_id);
	return 0;
}

int party_changeoption(struct map_session_data *sd,int exp,int item)
{
	nullpo_ret(sd);

	if( sd->status.party_id==0)
		return 0;
	intif->party_changeoption(sd->status.party_id,sd->status.account_id,exp,item);
	return 0;
}

int party_optionchanged(int party_id,int account_id,int exp,int item,int flag) {
	struct party_data *p;
	struct map_session_data *sd=map->id2sd(account_id);
	if( (p=party->search(party_id))==NULL)
		return 0;

	//Flag&1: Exp change denied. Flag&2: Item change denied.
	if(!(flag&0x01) && p->party.exp != exp)
		p->party.exp=exp;
	if(!(flag&0x10) && p->party.item != item) {
		p->party.item=item;
	}

	clif->party_option(p,sd,flag);
	return 0;
}

bool party_changeleader(struct map_session_data *sd, struct map_session_data *tsd)
{
	struct party_data *p;
	int mi, tmi;

	if (!sd || !sd->status.party_id)
		return false;

	if (!tsd || tsd->status.party_id != sd->status.party_id) {
		clif->message(sd->fd, msg_txt(283));
		return false;
	}

	if( map->list[sd->bl.m].flag.partylock ) {
		clif->message(sd->fd, msg_txt(287));
		return false;
	}

	if ((p = party->search(sd->status.party_id)) == NULL)
		return false;

	ARR_FIND( 0, MAX_PARTY, mi, p->data[mi].sd == sd );
	if (mi == MAX_PARTY)
		return false; //Shouldn't happen

	if (!p->party.member[mi].leader)
	{	//Need to be a party leader.
		clif->message(sd->fd, msg_txt(282));
		return false;
	}

	ARR_FIND( 0, MAX_PARTY, tmi, p->data[tmi].sd == tsd);
	if (tmi == MAX_PARTY)
		return false; //Shouldn't happen

	//Change leadership.
	p->party.member[mi].leader = 0;
	if (p->data[mi].sd->fd)
		clif->message(p->data[mi].sd->fd, msg_txt(284));

	p->party.member[tmi].leader = 1;
	if (p->data[tmi].sd->fd)
		clif->message(p->data[tmi].sd->fd, msg_txt(285));

	//Update info.
	intif->party_leaderchange(p->party.party_id,p->party.member[tmi].account_id,p->party.member[tmi].char_id);
	clif->party_info(p,NULL);
	return true;
}

/// Invoked (from char-server) when a party member
/// - changes maps
/// - logs in or out
/// - gains a level (disabled)
int party_recv_movemap(int party_id,int account_id,int char_id, unsigned short mapid,int online,int lv)
{
	struct party_member* m;
	struct party_data* p;
	int i;

	p = party->search(party_id);
	if( p == NULL )
		return 0;

	ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == account_id && p->party.member[i].char_id == char_id );
	if( i == MAX_PARTY )
	{
		ShowError("party_recv_movemap: char %d/%d not found in party %s (id:%d)",account_id,char_id,p->party.name,party_id);
		return 0;
	}

	m = &p->party.member[i];
	m->map = mapid;
	m->online = online;
	m->lv = lv;
	//Check if they still exist on this map server
	p->data[i].sd = party->sd_check(party_id, account_id, char_id);

	clif->party_info(p,NULL);
	return 0;
}

void party_send_movemap(struct map_session_data *sd)
{
	struct party_data *p;

	if( sd->status.party_id==0 )
		return;

	intif->party_changemap(sd,1);

	p=party->search(sd->status.party_id);
	if (!p) return;

	if(sd->state.connect_new) {
		//Note that this works because this function is invoked before connect_new is cleared.
		clif->party_info(p,sd);
		clif->party_member_info(p,sd);
	}

	if (sd->fd) { // synchronize minimap positions with the rest of the party
		int i;
		for(i=0; i < MAX_PARTY; i++) {
			if (p->data[i].sd &&
				p->data[i].sd != sd &&
				p->data[i].sd->bl.m == sd->bl.m)
			{
				clif->party_xy_single(sd->fd, p->data[i].sd);
				clif->party_xy_single(p->data[i].sd->fd, sd);
			}
		}
	}
	return;
}

void party_send_levelup(struct map_session_data *sd)
{
	intif->party_changemap(sd,1);
}

int party_send_logout(struct map_session_data *sd)
{
	struct party_data *p;
	int i;

	if(!sd->status.party_id)
		return 0;

	intif->party_changemap(sd,0);
	p=party->search(sd->status.party_id);
	if(!p) return 0;

	ARR_FIND( 0, MAX_PARTY, i, p->data[i].sd == sd );
	if( i < MAX_PARTY )
		memset(&p->data[i], 0, sizeof(p->data[0]));
	else
		ShowError("party_send_logout: Failed to locate member %d:%d in party %d!\n", sd->status.account_id, sd->status.char_id, p->party.party_id);

	return 1;
}

int party_send_message(struct map_session_data *sd,const char *mes,int len)
{
	if(sd->status.party_id==0)
		return 0;
	intif->party_message(sd->status.party_id,sd->status.account_id,mes,len);
	party->recv_message(sd->status.party_id,sd->status.account_id,mes,len);

	// Chat logging type 'P' / Party Chat
	logs->chat(LOG_CHAT_PARTY, sd->status.party_id, sd->status.char_id, sd->status.account_id, mapindex_id2name(sd->mapindex), sd->bl.x, sd->bl.y, NULL, mes);

	return 0;
}

int party_recv_message(int party_id,int account_id,const char *mes,int len)
{
	struct party_data *p;
	if( (p=party->search(party_id))==NULL)
		return 0;
	clif->party_message(p,account_id,mes,len);
	return 0;
}

int party_skill_check(struct map_session_data *sd, int party_id, uint16 skill_id, uint16 skill_lv)
{
	struct party_data *p;
	struct map_session_data *p_sd;
	int i;

	if(!party_id || (p=party->search(party_id))==NULL)
		return 0;
	switch(skill_id) {
		case TK_COUNTER: //Increase Triple Attack rate of Monks.
			if (!p->state.monk) return 0;
			break;
		case MO_COMBOFINISH: //Increase Counter rate of Star Gladiators
			if (!p->state.sg) return 0;
			break;
		case AM_TWILIGHT2: //Twilight Pharmacy, requires Super Novice
			return p->state.snovice;
		case AM_TWILIGHT3: //Twilight Pharmacy, Requires Taekwon
			return p->state.tk;
		default:
			return 0; //Unknown case?
	}

	for(i=0;i<MAX_PARTY;i++){
		if ((p_sd = p->data[i].sd) == NULL)
			continue;
		if (sd->bl.m != p_sd->bl.m)
			continue;
		switch(skill_id) {
			case TK_COUNTER: //Increase Triple Attack rate of Monks.
				if((p_sd->class_&MAPID_UPPERMASK) == MAPID_MONK
					&& pc->checkskill(p_sd,MO_TRIPLEATTACK)) {
					sc_start4(&p_sd->bl,SC_SKILLRATE_UP,100,MO_TRIPLEATTACK,
						50+50*skill_lv, //+100/150/200% rate
						0,0,skill->get_time(SG_FRIEND, 1));
				}
				break;
			case MO_COMBOFINISH: //Increase Counter rate of Star Gladiators
				if((p_sd->class_&MAPID_UPPERMASK) == MAPID_STAR_GLADIATOR
					&& sd->sc.data[SC_COUNTERKICK_READY]
					&& pc->checkskill(p_sd,SG_FRIEND)) {
					sc_start4(&p_sd->bl,SC_SKILLRATE_UP,100,TK_COUNTER,
						50+50*pc->checkskill(p_sd,SG_FRIEND), //+100/150/200% rate
						0,0,skill->get_time(SG_FRIEND, 1));
				}
				break;
		}
	}
	return 0;
}

int party_send_xy_timer(int tid, int64 tick, int id, intptr_t data) {
	struct party_data* p;

	DBIterator *iter = db_iterator(party->db);
	// for each existing party,
	for( p = dbi_first(iter); dbi_exists(iter); p = dbi_next(iter) )
	{
		int i;

		if( !p->party.count )
		{// no online party members so do not iterate
			continue;
		}

		// for each member of this party,
		for( i = 0; i < MAX_PARTY; i++ )
		{
			struct map_session_data* sd = p->data[i].sd;
			if( !sd || sd->bg_id ) continue;

			if( p->data[i].x != sd->bl.x || p->data[i].y != sd->bl.y )
			{// perform position update
				clif->party_xy(sd);
				p->data[i].x = sd->bl.x;
				p->data[i].y = sd->bl.y;
			}
			if (battle_config.party_hp_mode && p->data[i].hp != sd->battle_status.hp)
			{// perform hp update
				clif->party_hp(sd);
				p->data[i].hp = sd->battle_status.hp;
			}
		}
	}
	dbi_destroy(iter);

	return 0;
}

int party_send_xy_clear(struct party_data *p)
{
	int i;

	nullpo_ret(p);

	for(i=0;i<MAX_PARTY;i++){
		if(!p->data[i].sd) continue;
		p->data[i].hp = 0;
		p->data[i].x = 0;
		p->data[i].y = 0;
	}
	return 0;
}

// exp share and added zeny share [Valaris]
int party_exp_share(struct party_data* p, struct block_list* src, unsigned int base_exp, unsigned int job_exp, int zeny)
{
	struct map_session_data* sd[MAX_PARTY];
	unsigned int i, c;
#ifdef RENEWAL_EXP
	unsigned int job_exp_bonus, base_exp_bonus;
#endif

	nullpo_ret(p);

	// count the number of players eligible for exp sharing
	for (i = c = 0; i < MAX_PARTY; i++) {
		if( (sd[c] = p->data[i].sd) == NULL || sd[c]->bl.m != src->m || pc_isdead(sd[c]) || (battle_config.idle_no_share && pc_isidle(sd[c])) )
			continue;
		c++;
	}
	if (c < 1)
		return 0;

	base_exp/=c;
	job_exp/=c;
	zeny/=c;

	if (battle_config.party_even_share_bonus && c > 1) {
		double bonus = 100 + battle_config.party_even_share_bonus*(c-1);
		if (base_exp)
			base_exp = (unsigned int) cap_value(base_exp * bonus/100, 0, UINT_MAX);
		if (job_exp)
			job_exp = (unsigned int) cap_value(job_exp * bonus/100, 0, UINT_MAX);
		if (zeny)
			zeny = (unsigned int) cap_value(zeny * bonus/100, INT_MIN, INT_MAX);
	}

#ifdef RENEWAL_EXP
	base_exp_bonus = base_exp;
	job_exp_bonus  = job_exp;
#endif
	
	for (i = 0; i < c; i++) {
#ifdef RENEWAL_EXP
		if( !(src && src->type == BL_MOB && ((TBL_MOB*)src)->db->mexp) ){
			struct mob_data *md = (TBL_MOB*)src;
			int rate = pc->level_penalty_mod(md->level - (sd[i])->status.base_level, md->status.race, md->status.mode, 1);
			
			base_exp = (unsigned int)cap_value(base_exp_bonus * rate / 100, 1, UINT_MAX);
			job_exp = (unsigned int)cap_value(job_exp_bonus * rate / 100, 1, UINT_MAX);
		}
#endif
		pc->gainexp(sd[i], src, base_exp, job_exp, false);

		if (zeny) // zeny from mobs [Valaris]
			pc->getzeny(sd[i],zeny,LOG_TYPE_PICKDROP_MONSTER,NULL);
	}
	return 0;
}

//Does party loot. first_charid holds the charid of the player who has time priority to take the item.
int party_share_loot(struct party_data* p, struct map_session_data* sd, struct item* item_data, int first_charid)
{
	TBL_PC* target = NULL;
	int i;
	if (p && p->party.item&2 && (first_charid || !(battle_config.party_share_type&1)))
	{
		//item distribution to party members.
		if (battle_config.party_share_type&2)
		{	//Round Robin
			TBL_PC* psd;
			i = p->itemc;
			do {
				i++;
				if (i >= MAX_PARTY)
					i = 0;	// reset counter to 1st person in party so it'll stop when it reaches "itemc"

				if( (psd = p->data[i].sd) == NULL || sd->bl.m != psd->bl.m || pc_isdead(psd) || (battle_config.idle_no_share && pc_isidle(psd)) )
					continue;

				if (pc->additem(psd,item_data,item_data->amount,LOG_TYPE_PICKDROP_PLAYER))
					continue; //Chosen char can't pick up loot.

				//Successful pick.
				p->itemc = i;
				target = psd;
				break;
			} while (i != p->itemc);
		}
		else
		{	//Random pick
			TBL_PC* psd[MAX_PARTY];
			int count = 0;
			//Collect pick candidates
			for (i = 0; i < MAX_PARTY; i++) {
				if( (psd[count] = p->data[i].sd) == NULL || psd[count]->bl.m != sd->bl.m || pc_isdead(psd[count]) || (battle_config.idle_no_share && pc_isidle(psd[count])) )
					continue;

				count++;
			}
			while (count > 0) { //Pick a random member.
				i = rnd()%count;
				if (pc->additem(psd[i],item_data,item_data->amount,LOG_TYPE_PICKDROP_PLAYER))
				{	//Discard this receiver.
					psd[i] = psd[count-1];
					count--;
				} else { //Successful pick.
					target = psd[i];
					break;
				}
			}
		}
	}

	if (!target) {
		target = sd; //Give it to the char that picked it up
		if ((i=pc->additem(sd,item_data,item_data->amount,LOG_TYPE_PICKDROP_PLAYER)))
			return i;
	}

	if( p && battle_config.party_show_share_picker && battle_config.show_picker_item_type&(1<<itemdb_type(item_data->nameid)) )
		clif->party_show_picker(target, item_data);

	return 0;
}

int party_send_dot_remove(struct map_session_data *sd)
{
	if (sd->status.party_id)
		clif->party_xy_remove(sd);
	return 0;
}

// To use for Taekwon's "Fighting Chant"
// int c = 0;
// party_foreachsamemap(party->sub_count, sd, 0, &c);
int party_sub_count(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd = (TBL_PC *)bl;

	if (sd->state.autotrade)
		return 0;

	if (battle_config.idle_no_share && pc_isidle(sd))
		return 0;

	return 1;
}

/**
 * Arglist-based version of party_foreachsamemap
 * @see party_foreachsamemap
 */
int party_vforeachsamemap(int (*func)(struct block_list*,va_list), struct map_session_data *sd, int range, va_list ap) {
	struct party_data *p;
	int i;
	int x0,y0,x1,y1;
	struct block_list *list[MAX_PARTY];
	int blockcount=0;
	int total = 0; //Return value.

	nullpo_ret(sd);

	if((p=party->search(sd->status.party_id))==NULL)
		return 0;

	x0=sd->bl.x-range;
	y0=sd->bl.y-range;
	x1=sd->bl.x+range;
	y1=sd->bl.y+range;

	for(i=0;i<MAX_PARTY;i++)
	{
		struct map_session_data *psd = p->data[i].sd;
		if(!psd) continue;
		if(psd->bl.m!=sd->bl.m || !psd->bl.prev)
			continue;
		if(range &&
			(psd->bl.x<x0 || psd->bl.y<y0 ||
			 psd->bl.x>x1 || psd->bl.y>y1 ) )
			continue;
		list[blockcount++]=&psd->bl;
	}

	map->freeblock_lock();

	for(i=0;i<blockcount;i++) {
		va_list ap_copy;
		va_copy(ap_copy, ap);
		total += func(list[i], ap_copy);
		va_end(ap_copy);
	}

	map->freeblock_unlock();

	return total;
}

/**
 * Executes 'func' for each party member on the same map and within a 'range' cells area
 * @param func  Function to execute
 * @param sd    Reference character for party, map, area center
 * @param range Area size (0 = whole map)
 * @param ...   Adidtional parameters to pass to func()
 * @return Sum of the return values from func()
 */
int party_foreachsamemap(int (*func)(struct block_list*,va_list), struct map_session_data *sd, int range, ...) {
	va_list ap;
	int ret;
	va_start(ap, range);
	ret = party->vforeachsamemap(func, sd, range, ap);
	va_end(ap);
	return ret;
}

/*==========================================
 * Party Booking in KRO [Spiria]
 *------------------------------------------*/

struct party_booking_ad_info* create_party_booking_data(void) {
	struct party_booking_ad_info *pb_ad;
	CREATE(pb_ad, struct party_booking_ad_info, 1);
	pb_ad->index = party->booking_nextid++;
	return pb_ad;
}

void party_recruit_register(struct map_session_data *sd, short level, const char *notice) {
#ifdef PARTY_RECRUIT
	struct party_booking_ad_info *pb_ad;

	pb_ad = (struct party_booking_ad_info*)idb_get(party->booking_db, sd->status.char_id);

	if( pb_ad == NULL )
	{
		pb_ad = party->create_booking_data();
		idb_put(party->booking_db, sd->status.char_id, pb_ad);
	}
	else
	{// already registered
		clif->PartyRecruitRegisterAck(sd, 2);
		return;
	}

	memcpy(pb_ad->charname,sd->status.name,NAME_LENGTH);
	pb_ad->expiretime = (int)time(NULL);
 	pb_ad->p_detail.level = level;
	safestrncpy(pb_ad->p_detail.notice, notice, PB_NOTICE_LENGTH);

	clif->PartyRecruitRegisterAck(sd, 0);
	clif->PartyRecruitInsertNotify(sd, pb_ad); // Notice
#else
	return;
#endif
}

void party_booking_register(struct map_session_data *sd, short level, short mapid, short* job) {
#ifndef PARTY_RECRUIT
	struct party_booking_ad_info *pb_ad;
 	int i;
	
	pb_ad = (struct party_booking_ad_info*)idb_get(party->booking_db, sd->status.char_id);
	
	if( pb_ad == NULL )
	{
		pb_ad = party->create_booking_data();
		idb_put(party->booking_db, sd->status.char_id, pb_ad);
	}
	else
	{// already registered
		clif->PartyBookingRegisterAck(sd, 2);
		return;
	}
	
	memcpy(pb_ad->charname,sd->status.name,NAME_LENGTH);
	pb_ad->expiretime = (int)time(NULL);
 	pb_ad->p_detail.level = level;
	pb_ad->p_detail.mapid = mapid;
	
	for(i=0;i<PARTY_BOOKING_JOBS;i++)
		if(job[i] != 0xFF)
			pb_ad->p_detail.job[i] = job[i];
		else pb_ad->p_detail.job[i] = -1;
	
	clif->PartyBookingRegisterAck(sd, 0);
	clif->PartyBookingInsertNotify(sd, pb_ad); // Notice
#else
	return;
#endif
}

void party_recruit_update(struct map_session_data *sd, const char *notice) {
#ifdef PARTY_RECRUIT
	struct party_booking_ad_info *pb_ad;

	pb_ad = (struct party_booking_ad_info*)idb_get(party->booking_db, sd->status.char_id);

	if( pb_ad == NULL )
		return;

	pb_ad->expiretime = (int)time(NULL);// Update time.

	if (notice != NULL) {
		safestrncpy(pb_ad->p_detail.notice, notice, PB_NOTICE_LENGTH);
	}

	clif->PartyRecruitUpdateNotify(sd, pb_ad);
#else
	return;
#endif
}
void party_booking_update(struct map_session_data *sd, short* job) {
#ifndef PARTY_RECRUIT
	int i;
	struct party_booking_ad_info *pb_ad;
	
	pb_ad = (struct party_booking_ad_info*)idb_get(party->booking_db, sd->status.char_id);
	
	if( pb_ad == NULL )
		return;
	
	pb_ad->expiretime = (int)time(NULL);// Update time.
	
	for(i=0;i<PARTY_BOOKING_JOBS;i++)
		if(job[i] != 0xFF)
			pb_ad->p_detail.job[i] = job[i];
		else pb_ad->p_detail.job[i] = -1;
	
	clif->PartyBookingUpdateNotify(sd, pb_ad);
#else
	return;
#endif
}


void party_recruit_search(struct map_session_data *sd, short level, short mapid, unsigned long lastindex, short resultcount) {
#ifdef PARTY_RECRUIT
	struct party_booking_ad_info *pb_ad;
	int count = 0;
	struct party_booking_ad_info* result_list[PARTY_BOOKING_RESULTS];
	bool more_result = false;
	DBIterator* iter = db_iterator(party->booking_db);

	memset(result_list, 0, sizeof(result_list));

	for( pb_ad = dbi_first(iter); dbi_exists(iter); pb_ad = dbi_next(iter) )
	{
		if ((level && (pb_ad->p_detail.level < level-15 || pb_ad->p_detail.level > level)))
			continue;
		if (count >= PARTY_BOOKING_RESULTS){
			more_result = true;
			break;
		}
		result_list[count] = pb_ad;
		if( result_list[count] )
		{
			count++;
		}
	}
	dbi_destroy(iter);
	clif->PartyRecruitSearchAck(sd->fd, result_list, count, more_result);
#else
	return;
#endif
}
void party_booking_search(struct map_session_data *sd, short level, short mapid, short job, unsigned long lastindex, short resultcount) {
#ifndef PARTY_RECRUIT
	struct party_booking_ad_info *pb_ad;
	int i;
	int count = 0;
	struct party_booking_ad_info* result_list[PARTY_BOOKING_RESULTS];
	bool more_result = false;
	DBIterator* iter = db_iterator(party->booking_db);
	
	memset(result_list, 0, sizeof(result_list));
	
	for( pb_ad = dbi_first(iter); dbi_exists(iter); pb_ad = dbi_next(iter) ) {
		if (pb_ad->index < lastindex || (level && (pb_ad->p_detail.level < level-15 || pb_ad->p_detail.level > level)))
			continue;
		if (count >= PARTY_BOOKING_RESULTS){
			more_result = true;
			break;
		}
		if (mapid == 0 && job == -1)
			result_list[count] = pb_ad;
		else if (mapid == 0) {
			for(i=0; i<PARTY_BOOKING_JOBS; i++)
				if (pb_ad->p_detail.job[i] == job && job != -1)
					result_list[count] = pb_ad;
		} else if (job == -1){
			if (pb_ad->p_detail.mapid == mapid)
				result_list[count] = pb_ad;
		}
		if( result_list[count] )
		{
			count++;
		}
	}
	dbi_destroy(iter);
	clif->PartyBookingSearchAck(sd->fd, result_list, count, more_result);
#else
	return;
#endif
}


bool party_booking_delete(struct map_session_data *sd)
{
	struct party_booking_ad_info* pb_ad;

	if((pb_ad = (struct party_booking_ad_info*)idb_get(party->booking_db, sd->status.char_id))!=NULL)
	{
#ifdef PARTY_RECRUIT
		clif->PartyRecruitDeleteNotify(sd, pb_ad->index);
#else
		clif->PartyBookingDeleteNotify(sd, pb_ad->index);
#endif
		idb_remove(party->booking_db,sd->status.char_id);
	}
	return true;
}
void do_final_party(void) {
	party->db->destroy(party->db,party->db_final);
	db_destroy(party->booking_db); // Party Booking [Spiria]
}
// Constructor, init vars
void do_init_party(bool minimal) {
	if (minimal)
		return;

	party->db = idb_alloc(DB_OPT_RELEASE_DATA);
	party->booking_db = idb_alloc(DB_OPT_RELEASE_DATA); // Party Booking [Spiria]
	timer->add_func_list(party->send_xy_timer, "party_send_xy_timer");
	timer->add_interval(timer->gettick()+battle_config.party_update_interval, party->send_xy_timer, 0, 0, battle_config.party_update_interval);
}
/*=====================================
* Default Functions : party.h 
* Generated by HerculesInterfaceMaker
* created by Susu
*-------------------------------------*/
void party_defaults(void) {
	party = &party_s;

	/* */
	party->db = NULL;
	party->booking_db = NULL;
	party->booking_nextid = 1;
	/* funcs */
	party->init = do_init_party;
	party->final = do_final_party;
	/* */
	party->search = party_search;
	party->searchname = party_searchname;
	party->getmemberid = party_getmemberid;
	party->getavailablesd = party_getavailablesd;
	
	party->create = party_create;
	party->created = party_created;
	party->request_info = party_request_info;
	party->invite = party_invite;
	party->member_joined = party_member_joined;
	party->member_added = party_member_added;
	party->leave = party_leave;
	party->removemember = party_removemember;
	party->member_withdraw = party_member_withdraw;
	party->reply_invite = party_reply_invite;
	party->recv_noinfo = party_recv_noinfo;
	party->recv_info = party_recv_info;
	party->recv_movemap = party_recv_movemap;
	party->broken = party_broken;
	party->optionchanged = party_optionchanged;
	party->changeoption = party_changeoption;
	party->changeleader = party_changeleader;
	party->send_movemap = party_send_movemap;
	party->send_levelup = party_send_levelup;
	party->send_logout = party_send_logout;
	party->send_message = party_send_message;
	party->recv_message = party_recv_message;
	party->skill_check = party_skill_check;
	party->send_xy_clear = party_send_xy_clear;
	party->exp_share = party_exp_share;
	party->share_loot = party_share_loot;
	party->send_dot_remove = party_send_dot_remove;
	party->sub_count = party_sub_count;
	party->booking_register = party_booking_register;
	party->booking_update = party_booking_update;
	party->booking_search = party_booking_search;
	party->recruit_register = party_recruit_register;
	party->recruit_update = party_recruit_update;
	party->recruit_search = party_recruit_search;
	party->booking_delete = party_booking_delete;
	party->vforeachsamemap = party_vforeachsamemap;
	party->foreachsamemap = party_foreachsamemap;
	party->send_xy_timer = party_send_xy_timer;
	party->fill_member = party_fill_member;
	party->sd_check = party_sd_check;
	party->check_state = party_check_state;
	party->create_booking_data = create_party_booking_data;
	party->db_final = party_db_final;
}
