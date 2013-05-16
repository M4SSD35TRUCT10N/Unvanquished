/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Daemon.

Daemon is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Daemon is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "g_bot_parse.h"
#include "g_bot_util.h"

static botMemory_t g_botMind[MAX_CLIENTS];
static AITreeList_t treeList;

/*
=======================
Bot management functions
=======================
*/
static struct
{
	int count;
	struct
	{
		char *name;
		qboolean inUse;
	} name[MAX_CLIENTS];
} botNames[NUM_TEAMS];

static void G_BotListTeamNames( gentity_t *ent, const char *heading, team_t team, const char *marker )
{
	int i;

	if ( botNames[team].count )
	{
		ADMP( heading );
		ADMBP_begin();

		for ( i = 0; i < botNames[team].count; ++i )
		{
			ADMBP( va( "  %s^7 %s\n", botNames[team].name[i].inUse ? marker : " ", botNames[team].name[i].name ) );
		}

		ADMBP_end();
	}
}

void G_BotListNames( gentity_t *ent )
{
	G_BotListTeamNames( ent, QQ( N_( "^3Alien bot names:\n" ) ), TEAM_ALIENS, "^1*" );
	G_BotListTeamNames( ent, QQ( N_( "^3Human bot names:\n" ) ), TEAM_HUMANS, "^5*" );
}

qboolean G_BotClearNames( void )
{
	int i;

	for ( i = 0; i < botNames[TEAM_ALIENS].count; ++i )
		if ( botNames[TEAM_ALIENS].name[i].inUse )
		{
			return qfalse;
		}

	for ( i = 0; i < botNames[TEAM_HUMANS].count; ++i )
		if ( botNames[TEAM_HUMANS].name[i].inUse )
		{
			return qfalse;
		}

	for ( i = 0; i < botNames[TEAM_ALIENS].count; ++i )
	{
		BG_Free( botNames[TEAM_ALIENS].name[i].name );
	}

	for ( i = 0; i < botNames[TEAM_HUMANS].count; ++i )
	{
		BG_Free( botNames[TEAM_HUMANS].name[i].name );
	}

	botNames[TEAM_ALIENS].count = 0;
	botNames[TEAM_HUMANS].count = 0;

	return qtrue;
}

int G_BotAddNames( team_t team, int arg, int last )
{
	int  i = botNames[team].count;
	int  added = 0;
	char name[MAX_NAME_LENGTH];

	while ( arg < last && i < MAX_CLIENTS )
	{
		int j, t;

		trap_Argv( arg++, name, sizeof( name ) );

		// name already in the list? (quick check, including colours & invalid)
		for ( t = 1; t < NUM_TEAMS; ++t )
			for ( j = 0; j < botNames[t].count; ++j )
				if ( !Q_stricmp( botNames[t].name[j].name, name ) )
				{
					goto next;
				}

		botNames[team].name[i].name = ( char * )BG_Alloc( strlen( name ) + 1 );
		strcpy( botNames[team].name[i].name, name );

		botNames[team].count = ++i;
		++added;

next:
		;
	}

	return added;
}

static char *G_BotSelectName( team_t team )
{
	unsigned int i, choice;

	if ( botNames[team].count < 1 )
	{
		return NULL;
	}

	choice = rand() % botNames[team].count;

	for ( i = 0; i < botNames[team].count; ++i )
	{
		if ( !botNames[team].name[choice].inUse )
		{
			return botNames[team].name[choice].name;
		}
		choice = ( choice + 1 ) % botNames[team].count;
	}

	return NULL;
}

static void G_BotNameUsed( team_t team, const char *name, qboolean inUse )
{
	unsigned int i;

	for ( i = 0; i < botNames[team].count; ++i )
	{
		if ( !Q_stricmp( name, botNames[team].name[i].name ) )
		{
			botNames[team].name[i].inUse = inUse;
			return;
		}
	}
}

qboolean G_BotSetDefaults( int clientNum, team_t team, int skill, const char* behavior )
{
	botMemory_t *botMind;
	gentity_t *self = &g_entities[ clientNum ];
	botMind = self->botMind = &g_botMind[clientNum];

	botMind->enemyLastSeen = 0;
	botMind->botTeam = team;
	BotSetNavmesh( self, self->client->ps.stats[ STAT_CLASS ] );

	memset( botMind->runningNodes, 0, sizeof( botMind->runningNodes ) );
	botMind->numRunningNodes = 0;
	botMind->currentNode = NULL;
	botMind->directPathToGoal = qfalse;

	botMind->behaviorTree = ReadBehaviorTree( behavior, &treeList );

	if ( !botMind->behaviorTree )
	{
		G_Printf( "Problem when loading behavior tree %s, trying default\n", behavior );
		botMind->behaviorTree = ReadBehaviorTree( "default", &treeList );

		if ( !botMind->behaviorTree )
		{
			G_Printf( "Problem when loading default behavior tree\n" );
			return qfalse;
		}
	}

	BotSetSkillLevel( &g_entities[clientNum], skill );

	g_entities[clientNum].r.svFlags |= SVF_BOT;

	if ( team != TEAM_NONE )
	{
		level.clients[clientNum].sess.restartTeam = team;
	}

	self->pain = BotPain;

	return qtrue;
}

qboolean G_BotAdd( char *name, team_t team, int skill, const char *behavior )
{
	int clientNum;
	char userinfo[MAX_INFO_STRING];
	const char* s = 0;
	gentity_t *bot;
	qboolean autoname = qfalse;
	qboolean okay;

	if ( !navMeshLoaded )
	{
		trap_Print( "No Navigation Mesh file is available for this map\n" );
		return qfalse;
	}

	// find what clientNum to use for bot
	clientNum = trap_BotAllocateClient( 0 );

	if ( clientNum < 0 )
	{
		trap_Print( "no more slots for bot\n" );
		return qfalse;
	}
	bot = &g_entities[ clientNum ];
	bot->r.svFlags |= SVF_BOT;
	bot->inuse = qtrue;

	if ( !Q_stricmp( name, "*" ) )
	{
		name = G_BotSelectName( team );
		autoname = name != NULL;
	}

	//default bot data
	okay = G_BotSetDefaults( clientNum, team, skill, behavior );

	// register user information
	userinfo[0] = '\0';
	Info_SetValueForKey( userinfo, "name", name ? name : "", qfalse ); // allow defaulting
	Info_SetValueForKey( userinfo, "rate", "25000", qfalse );
	Info_SetValueForKey( userinfo, "snaps", "20", qfalse );
	if ( autoname )
	{
		Info_SetValueForKey( userinfo, "autoname", name, qfalse );
	}

	//so we can connect if server is password protected
	if ( g_needpass.integer == 1 )
	{
		Info_SetValueForKey( userinfo, "password", g_password.string, qfalse );
	}

	trap_SetUserinfo( clientNum, userinfo );

	// have it connect to the game as a normal client
	if ( ( s = ClientBotConnect( clientNum, qtrue, team ) ) )
	{
		// won't let us join
		trap_Print( s );
		okay = qfalse;
	}

	if ( !okay )
	{
		G_BotDel( clientNum );
		return qfalse;
	}

	if ( autoname )
	{
		G_BotNameUsed( team, name, qtrue );
	}

	ClientBegin( clientNum );
	bot->pain = BotPain; // ClientBegin resets the pain function
	return qtrue;
}

void G_BotDel( int clientNum )
{
	gentity_t *bot = &g_entities[clientNum];
	char userinfo[MAX_INFO_STRING];
	const char *autoname;

	if ( !( bot->r.svFlags & SVF_BOT ) || !bot->botMind )
	{
		trap_Print( va( "'^7%s^7' is not a bot\n", bot->client->pers.netname ) );
		return;
	}

	trap_GetUserinfo( clientNum, userinfo, sizeof( userinfo ) );

	autoname = Info_ValueForKey( userinfo, "autoname" );
	if ( autoname && *autoname )
	{
		G_BotNameUsed( BotGetEntityTeam( bot ), autoname, qfalse );
	}

	trap_SendServerCommand( -1, va( "print_tr %s %s", QQ( N_( "$1$^7 disconnected\n" ) ),
	                                Quote( bot->client->pers.netname ) ) );
	trap_DropClient( clientNum, "disconnected" );
}

void G_BotDelAllBots( void )
{
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( g_entities[i].r.svFlags & SVF_BOT && level.clients[i].pers.connected != CON_DISCONNECTED )
		{
			G_BotDel( i );
		}
	}

	for ( i = 0; i < botNames[TEAM_ALIENS].count; ++i )
	{
		botNames[TEAM_ALIENS].name[i].inUse = qfalse;
	}

	for ( i = 0; i < botNames[TEAM_HUMANS].count; ++i )
	{
		botNames[TEAM_HUMANS].name[i].inUse = qfalse;
	}
}

/*
=======================
Bot Thinks
=======================
*/

void G_BotThink( gentity_t *self )
{
	char buf[MAX_STRING_CHARS];
	usercmd_t *botCmdBuffer;
	vec3_t     nudge;
	gentity_t *bestDamaged;
	botRouteTarget_t routeTarget;

	self->botMind->cmdBuffer = self->client->pers.cmd;
	botCmdBuffer = &self->botMind->cmdBuffer;

	//reset command buffer
	usercmdClearButtons( botCmdBuffer->buttons );

	// for nudges, e.g. spawn blocking
	nudge[0] = botCmdBuffer->doubleTap ? botCmdBuffer->forwardmove : 0;
	nudge[1] = botCmdBuffer->doubleTap ? botCmdBuffer->rightmove : 0;
	nudge[2] = botCmdBuffer->doubleTap ? botCmdBuffer->upmove : 0;

	botCmdBuffer->forwardmove = 0;
	botCmdBuffer->rightmove = 0;
	botCmdBuffer->upmove = 0;
	botCmdBuffer->doubleTap = 0;

	//acknowledge recieved server commands
	//MUST be done
	while ( trap_BotGetServerCommand( self->client->ps.clientNum, buf, sizeof( buf ) ) );

	BotSearchForEnemy( self );

	BotFindClosestBuildings( self, self->botMind->closestBuildings );

	bestDamaged = BotFindDamagedFriendlyStructure( self );
	self->botMind->closestDamagedBuilding.ent = bestDamaged;
	self->botMind->closestDamagedBuilding.distance = ( !bestDamaged ) ? 0 :  Distance( self->s.origin, bestDamaged->s.origin );

	//use medkit when hp is low
	if ( self->health < BOT_USEMEDKIT_HP && BG_InventoryContainsUpgrade( UP_MEDKIT, self->client->ps.stats ) )
	{
		BG_ActivateUpgrade( UP_MEDKIT, self->client->ps.stats );
	}

	//infinite funds cvar
	if ( g_bot_infinite_funds.integer )
	{
		G_AddCreditToClient( self->client, HUMAN_MAX_CREDITS, qtrue );
	}

	//hacky ping fix
	self->client->ps.ping = rand() % 50 + 50;

	if ( !self->botMind->behaviorTree )
	{
		G_Printf( "ERROR: NULL behavior tree\n" );
		return;
	}

	// always update the path corridor
	if ( self->botMind->goal.inuse )
	{
		BotTargetToRouteTarget( self, self->botMind->goal, &routeTarget );
		trap_BotUpdatePath( self->s.number, &routeTarget, NULL, &self->botMind->directPathToGoal );
	}
	
	BotEvaluateNode( self, ( AIGenericNode_t * ) self->botMind->behaviorTree->root );

	// if we were nudged...
	VectorAdd( self->client->ps.velocity, nudge, self->client->ps.velocity );

	self->client->pers.cmd = self->botMind->cmdBuffer;
}

void G_BotSpectatorThink( gentity_t *self )
{
	char buf[MAX_STRING_CHARS];
	//hacky ping fix
	self->client->ps.ping = rand() % 50 + 50;

	//acknowledge recieved console messages
	//MUST be done
	while ( trap_BotGetServerCommand( self->client->ps.clientNum, buf, sizeof( buf ) ) );

	if ( self->client->ps.pm_flags & PMF_QUEUED )
	{
		//we're queued to spawn, all good
		//check for humans in the spawn que
		{
			spawnQueue_t *sq;
			if ( self->client->pers.teamSelection == TEAM_HUMANS )
			{
				sq = &level.humanSpawnQueue;
			}
			else if ( self->client->pers.teamSelection == TEAM_ALIENS )
			{
				sq = &level.alienSpawnQueue;
			}
			else
			{
				sq = NULL;
			}

			if ( sq && PlayersBehindBotInSpawnQueue( self ) )
			{
				G_RemoveFromSpawnQueue( sq, self->s.number );
				G_PushSpawnQueue( sq, self->s.number );
			}
		}
		return;
	}

	//reset stuff
	BotSetTarget( &self->botMind->goal, NULL, NULL );
	self->botMind->bestEnemy.ent = NULL;
	self->botMind->timeFoundEnemy = 0;
	self->botMind->enemyLastSeen = 0;
	self->botMind->currentNode = NULL;
	self->botMind->directPathToGoal = qfalse;
	self->botMind->futureAimTime = 0;
	self->botMind->futureAimTimeInterval = 0;
	self->botMind->numRunningNodes = 0;
	memset( self->botMind->runningNodes, 0, sizeof( self->botMind->runningNodes ) );

	if ( self->client->sess.restartTeam == TEAM_NONE )
	{
		int teamnum = self->client->pers.teamSelection;
		int clientNum = self->client->ps.clientNum;

		if ( teamnum == TEAM_HUMANS )
		{
			self->client->pers.classSelection = PCL_HUMAN;
			self->client->ps.stats[STAT_CLASS] = PCL_HUMAN;
			BotSetNavmesh( self, PCL_HUMAN );
			//we want to spawn with rifle unless it is disabled or we need to build
			if ( g_bot_rifle.integer )
			{
				self->client->pers.humanItemSelection = WP_MACHINEGUN;
			}
			else
			{
				self->client->pers.humanItemSelection = WP_HBUILD;
			}

			G_PushSpawnQueue( &level.humanSpawnQueue, clientNum );
		}
		else if ( teamnum == TEAM_ALIENS )
		{
			self->client->pers.classSelection = PCL_ALIEN_LEVEL0;
			self->client->ps.stats[STAT_CLASS] = PCL_ALIEN_LEVEL0;
			BotSetNavmesh( self, PCL_ALIEN_LEVEL0 );
			G_PushSpawnQueue( &level.alienSpawnQueue, clientNum );
		}
	}
	else
	{
		G_ChangeTeam( self, self->client->sess.restartTeam );
		self->client->sess.restartTeam = TEAM_NONE;
	}
}

void G_BotIntermissionThink( gclient_t *client )
{
	client->readyToExit = qtrue;
}

void G_BotInit( void )
{
	G_BotNavInit( );
	if ( treeList.maxTrees == 0 )
	{
		InitTreeList( &treeList );
	}
}

void G_BotCleanup( int restart )
{
	if ( !restart )
	{
		int i;

		for ( i = 0; i < MAX_CLIENTS; ++i )
		{
			if ( g_entities[i].r.svFlags & SVF_BOT && level.clients[i].pers.connected != CON_DISCONNECTED )
			{
				G_BotDel( i );
			}
		}

		G_BotClearNames();
	}
	FreeTreeList( &treeList );
	G_BotNavCleanup();
}
