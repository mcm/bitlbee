  /********************************************************************\
  * BitlBee -- An IRC to other IM-networks gateway                     *
  *                                                                    *
  * Copyright 2002-2010 Wilmer van der Gaast and others                *
  \********************************************************************/

/* IRC commands                                                         */

/*
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License with
  the Debian GNU/Linux distribution in /usr/share/common-licenses/GPL;
  if not, write to the Free Software Foundation, Inc., 59 Temple Place,
  Suite 330, Boston, MA  02111-1307  USA
*/

#define BITLBEE_CORE
#include "bitlbee.h"
#include "ipc.h"
#include "chat.h"

static void irc_cmd_pass( irc_t *irc, char **cmd )
{
	if( irc->status & USTATUS_LOGGED_IN )
	{
		char *send_cmd[] = { "identify", cmd[1], NULL };
		
		/* We're already logged in, this client seems to send the PASS
		   command last. (Possibly it won't send it at all if it turns
		   out we don't require it, which will break this feature.)
		   Try to identify using the given password. */
		return root_command( irc, send_cmd );
	}
	/* Handling in pre-logged-in state, first see if this server is
	   password-protected: */
	else if( global.conf->auth_pass &&
	    ( strncmp( global.conf->auth_pass, "md5:", 4 ) == 0 ?
	        md5_verify_password( cmd[1], global.conf->auth_pass + 4 ) == 0 :
	        strcmp( cmd[1], global.conf->auth_pass ) == 0 ) )
	{
		irc->status |= USTATUS_AUTHORIZED;
		irc_check_login( irc );
	}
	else if( global.conf->auth_pass )
	{
		irc_send_num( irc, 464, ":Incorrect password" );
	}
	else
	{
		/* Remember the password and try to identify after USER/NICK. */
		/*irc_setpass( irc, cmd[1] ); */
		irc_check_login( irc );
	}
}

static void irc_cmd_user( irc_t *irc, char **cmd )
{
	irc->user->user = g_strdup( cmd[1] );
	irc->user->fullname = g_strdup( cmd[4] );
	
	irc_check_login( irc );
}

static void irc_cmd_nick( irc_t *irc, char **cmd )
{
	if( irc_user_by_name( irc, cmd[1] ) )
	{
		irc_send_num( irc, 433, ":This nick is already in use" );
	}
	else if( !nick_ok( cmd[1] ) )
	{
		/* [SH] Invalid characters. */
		irc_send_num( irc, 432, ":This nick contains invalid characters" );
	}
	else if( irc->user->nick )
	{
		if( irc->status & USTATUS_IDENTIFIED )
		{
			irc_setpass( irc, NULL );
			irc->status &= ~USTATUS_IDENTIFIED;
			irc_umode_set( irc, "-R", 1 );
		}
		
		irc_user_set_nick( irc->user, cmd[1] );
	}
	else
	{
		irc->user->nick = g_strdup( cmd[1] );
		
		irc_check_login( irc );
	}
}

static void irc_cmd_quit( irc_t *irc, char **cmd )
{
	if( cmd[1] && *cmd[1] )
		irc_abort( irc, 0, "Quit: %s", cmd[1] );
	else
		irc_abort( irc, 0, "Leaving..." );
}

static void irc_cmd_ping( irc_t *irc, char **cmd )
{
	irc_write( irc, ":%s PONG %s :%s", irc->root->host,
	           irc->root->host, cmd[1]?cmd[1]:irc->root->host );
}

static void irc_cmd_pong( irc_t *irc, char **cmd )
{
	/* We could check the value we get back from the user, but in
	   fact we don't care, we're just happy s/he's still alive. */
	irc->last_pong = gettime();
	irc->pinging = 0;
}

static void irc_cmd_join( irc_t *irc, char **cmd )
{
	irc_channel_t *ic;
	
	if( ( ic = irc_channel_by_name( irc, cmd[1] ) ) == NULL )
		ic = irc_channel_new( irc, cmd[1] );
	
	if( ic == NULL )
	{
		irc_send_num( irc, 479, "%s :Invalid channel name", cmd[1] );
		return;
	}
	
	if( ic->flags & IRC_CHANNEL_JOINED )
		return; /* Dude, you're already there...
		           RFC doesn't have any reply for that though? */
	
	if( ic->f->join && !ic->f->join( ic ) )
		/* The story is: FALSE either means the handler showed an error
		   message, or is doing some work before the join should be
		   confirmed. (In the latter case, the caller should take care
		   of that confirmation.)
		   TRUE means all's good, let the user join the channel right away. */
		return;
	
	irc_channel_add_user( ic, irc->user );
}

static void irc_cmd_names( irc_t *irc, char **cmd )
{
	irc_channel_t *ic;
	
	if( cmd[1] && ( ic = irc_channel_by_name( irc, cmd[1] ) ) )
		irc_send_names( ic );
	/* With no args, we should show /names of all chans. Make the code
	   below work well if necessary.
	else
	{
		GSList *l;
		
		for( l = irc->channels; l; l = l->next )
			irc_send_names( l->data );
	}
	*/
}

static void irc_cmd_part( irc_t *irc, char **cmd )
{
	irc_channel_t *ic;
	
	if( ( ic = irc_channel_by_name( irc, cmd[1] ) ) == NULL )
	{
		irc_send_num( irc, 403, "%s :No such channel", cmd[1] );
	}
	else if( irc_channel_del_user( ic, irc->user ) )
	{
		if( ic->f->part )
			ic->f->part( ic, NULL );
	}
	else
	{
		irc_send_num( irc, 442, "%s :You're not on that channel", cmd[1] );
	}
}

static void irc_cmd_whois( irc_t *irc, char **cmd )
{
	char *nick = cmd[1];
	irc_user_t *iu = irc_user_by_name( irc, nick );
	
	if( iu )
		irc_send_whois( iu );
	else
		irc_send_num( irc, 401, "%s :Nick does not exist", nick );
}

static void irc_cmd_whowas( irc_t *irc, char **cmd )
{
	/* For some reason irssi tries a whowas when whois fails. We can
	   ignore this, but then the user never gets a "user not found"
	   message from irssi which is a bit annoying. So just respond
	   with not-found and irssi users will get better error messages */
	
	irc_send_num( irc, 406, "%s :Nick does not exist", cmd[1] );
	irc_send_num( irc, 369, "%s :End of WHOWAS", cmd[1] );
}

static void irc_cmd_motd( irc_t *irc, char **cmd )
{
	irc_send_motd( irc );
}

static void irc_cmd_mode( irc_t *irc, char **cmd )
{
	if( irc_channel_name_ok( cmd[1] ) )
	{
		irc_channel_t *ic;
		
		if( ( ic = irc_channel_by_name( irc, cmd[1] ) ) == NULL )
			irc_send_num( irc, 403, "%s :No such channel", cmd[1] );
		else if( cmd[2] )
		{
			if( *cmd[2] == '+' || *cmd[2] == '-' )
				irc_send_num( irc, 477, "%s :Can't change channel modes", cmd[1] );
			else if( *cmd[2] == 'b' )
				irc_send_num( irc, 368, "%s :No bans possible", cmd[1] );
		}
		else
			irc_send_num( irc, 324, "%s +%s", cmd[1], ic->mode );
	}
	else
	{
		if( nick_cmp( cmd[1], irc->user->nick ) == 0 )
		{
			if( cmd[2] )
				irc_umode_set( irc, cmd[2], 0 );
			else
				irc_send_num( irc, 221, "+%s", irc->umode );
		}
		else
			irc_send_num( irc, 502, ":Don't touch their modes" );
	}
}

static void irc_cmd_who( irc_t *irc, char **cmd )
{
	char *channel = cmd[1];
	irc_channel_t *ic;
	
	if( !channel || *channel == '0' || *channel == '*' || !*channel )
		irc_send_who( irc, irc->users, "**" );
	else if( ( ic = irc_channel_by_name( irc, channel ) ) )
		irc_send_who( irc, ic->users, channel );
	else
		irc_send_num( irc, 403, "%s :No such channel", channel );
}

static void irc_cmd_privmsg( irc_t *irc, char **cmd )
{
	irc_channel_t *ic;
	irc_user_t *iu;
	
	if( !cmd[2] ) 
	{
		irc_send_num( irc, 412, ":No text to send" );
		return;
	}
	
	/* Don't treat CTCP actions as real CTCPs, just convert them right now. */
	if( g_strncasecmp( cmd[2], "\001ACTION", 7 ) == 0 )
	{
		cmd[2] += 4;
		strcpy( cmd[2], "/me" );
		if( cmd[2][strlen(cmd[2])-1] == '\001' )
			cmd[2][strlen(cmd[2])-1] = '\0';
	}
	
	if( irc_channel_name_ok( cmd[1] ) &&
	    ( ic = irc_channel_by_name( irc, cmd[1] ) ) )
	{
		if( ic->f->privmsg )
			ic->f->privmsg( ic, cmd[2] );
	}
	else if( ( iu = irc_user_by_name( irc, cmd[1] ) ) )
	{
		if( cmd[2][0] == '\001' )
		{
			char **ctcp;
			
			if( iu->f->ctcp == NULL )
				return;
			if( cmd[2][strlen(cmd[2])-1] == '\001' )
				cmd[2][strlen(cmd[2])-1] = '\0';
			
			ctcp = split_command_parts( cmd[2] + 1 );
			iu->f->ctcp( iu, ctcp );
		}
		else if( iu->f->privmsg )
		{
			iu->flags |= IRC_USER_PRIVATE;
			iu->f->privmsg( iu, cmd[2] );
		}
	}
	else
	{
		irc_send_num( irc, 401, "%s :No such nick/channel", cmd[1] );
	}


#if 0
	else if( irc->nick && g_strcasecmp( cmd[1], irc->nick ) == 0 ) 
	{
	}
	else 
	{
		if( g_strcasecmp( cmd[1], irc->channel ) == 0 )
		{
			unsigned int i;
			char *t = set_getstr( &irc->set, "default_target" );
			
			if( g_strcasecmp( t, "last" ) == 0 && irc->last_target )
				cmd[1] = irc->last_target;
			else if( g_strcasecmp( t, "root" ) == 0 )
				cmd[1] = irc->mynick;
			
			for( i = 0; i < strlen( cmd[2] ); i ++ )
			{
				if( cmd[2][i] == ' ' ) break;
				if( cmd[2][i] == ':' || cmd[2][i] == ',' )
				{
					cmd[1] = cmd[2];
					cmd[2] += i;
					*cmd[2] = 0;
					while( *(++cmd[2]) == ' ' );
					break;
				}
			}
			
			irc->is_private = 0;
			
			if( cmd[1] != irc->last_target )
			{
				g_free( irc->last_target );
				irc->last_target = g_strdup( cmd[1] );
			}
		}
		else
		{
			irc->is_private = 1;
		}
		irc_send( irc, cmd[1], cmd[2], ( g_strcasecmp( cmd[0], "NOTICE" ) == 0 ) ? OPT_AWAY : 0 );
	}
#endif
}

static void irc_cmd_nickserv( irc_t *irc, char **cmd )
{
	/* [SH] This aliases the NickServ command to PRIVMSG root */
	/* [TV] This aliases the NS command to PRIVMSG root as well */
	root_command( irc, cmd + 1 );
}



static void irc_cmd_oper( irc_t *irc, char **cmd )
{
	if( global.conf->oper_pass &&
	    ( strncmp( global.conf->oper_pass, "md5:", 4 ) == 0 ?
	        md5_verify_password( cmd[2], global.conf->oper_pass + 4 ) == 0 :
	        strcmp( cmd[2], global.conf->oper_pass ) == 0 ) )
	{
		irc_umode_set( irc, "+o", 1 );
		irc_send_num( irc, 381, ":Password accepted" );
	}
	else
	{
		irc_send_num( irc, 432, ":Incorrect password" );
	}
}

static void irc_cmd_invite( irc_t *irc, char **cmd )
{
	irc_channel_t *ic;
	irc_user_t *iu;
	
	if( ( iu = irc_user_by_name( irc, cmd[1] ) ) == NULL )
	{
		irc_send_num( irc, 401, "%s :No such nick", cmd[1] );
		return;
	}
	else if( ( ic = irc_channel_by_name( irc, cmd[2] ) ) == NULL )
	{
		irc_send_num( irc, 403, "%s :No such channel", cmd[2] );
		return;
	}
	
	if( !ic->f->invite || !ic->f->invite( ic, iu ) )
		irc_send_num( irc, 482, "%s :Can't invite people here", cmd[2] );
}

static void irc_cmd_userhost( irc_t *irc, char **cmd )
{
	int i;
	
	/* [TV] Usable USERHOST-implementation according to
		RFC1459. Without this, mIRC shows an error
		while connecting, and the used way of rejecting
		breaks standards.
	*/
	
	for( i = 1; cmd[i]; i ++ )
	{
		irc_user_t *iu = irc_user_by_name( irc, cmd[i] );
		
		if( iu )
			irc_send_num( irc, 302, ":%s=%c%s@%s", iu->nick,
			              irc_user_get_away( iu ) ? '-' : '+',
			              iu->user, iu->host );
	}
}

static void irc_cmd_ison( irc_t *irc, char **cmd )
{
	char buff[IRC_MAX_LINE];
	int lenleft, i;
	
	buff[0] = '\0';
	
	/* [SH] Leave room for : and \0 */
	lenleft = IRC_MAX_LINE - 2;
	
	for( i = 1; cmd[i]; i ++ )
	{
		char *this, *next;
		
		this = cmd[i];
		while( *this )
		{
			irc_user_t *iu;
			
			if( ( next = strchr( this, ' ' ) ) )
				*next = 0;
			
			if( ( iu = irc_user_by_name( irc, this ) ) &&
			    iu->bu && iu->bu->flags & BEE_USER_ONLINE )
			{
				lenleft -= strlen( iu->nick ) + 1;
				
				if( lenleft < 0 )
					break;
				
				strcat( buff, iu->nick );
				strcat( buff, " " );
			}
			
			if( next )
			{
				*next = ' ';
				this = next + 1;
			}
			else
			{
				break;
			}    
		}
		
		/* *sigh* */
		if( lenleft < 0 )
			break;
	}
	
	if( strlen( buff ) > 0 )
		buff[strlen(buff)-1] = '\0';
	
	irc_send_num( irc, 303, ":%s", buff );
}

static void irc_cmd_watch( irc_t *irc, char **cmd )
{
	int i;
	
	/* Obviously we could also mark a user structure as being
	   watched, but what if the WATCH command is sent right
	   after connecting? The user won't exist yet then... */
	for( i = 1; cmd[i]; i ++ )
	{
		char *nick;
		irc_user_t *iu;
		
		if( !cmd[i][0] || !cmd[i][1] )
			break;
		
		nick = g_strdup( cmd[i] + 1 );
		nick_lc( nick );
		
		iu = irc_user_by_name( irc, nick );
		
		if( cmd[i][0] == '+' )
		{
			if( !g_hash_table_lookup( irc->watches, nick ) )
				g_hash_table_insert( irc->watches, nick, nick );
			
			if( iu && iu->bu && iu->bu->flags & BEE_USER_ONLINE )
				irc_send_num( irc, 604, "%s %s %s %d :%s", iu->nick, iu->user,
				              iu->host, (int) time( NULL ), "is online" );
			else
				irc_send_num( irc, 605, "%s %s %s %d :%s", nick, "*", "*",
				              (int) time( NULL ), "is offline" );
		}
		else if( cmd[i][0] == '-' )
		{
			gpointer okey, ovalue;
			
			if( g_hash_table_lookup_extended( irc->watches, nick, &okey, &ovalue ) )
			{
				g_hash_table_remove( irc->watches, okey );
				g_free( okey );
				
				irc_send_num( irc, 602, "%s %s %s %d :%s", nick, "*", "*", 0, "Stopped watching" );
			}
		}
	}
}

static void irc_cmd_topic( irc_t *irc, char **cmd )
{
	irc_channel_t *ic = irc_channel_by_name( irc, cmd[1] );
	const char *new = cmd[2];
	
	if( ic == NULL )
	{
		irc_send_num( irc, 403, "%s :No such channel", cmd[1] );
	}
	else if( new )
	{
		if( ic->f->topic == NULL )
			irc_send_num( irc, 482, "%s :Can't change this channel's topic", ic->name );
		else if( ic->f->topic( ic, new ) )
			irc_send_topic( ic, TRUE );
	}
	else
	{
		irc_send_topic( ic, FALSE );
	}
}

static void irc_cmd_away( irc_t *irc, char **cmd )
{
	if( cmd[1] && *cmd[1] )
	{
		char away[strlen(cmd[1])+1];
		int i, j;
		
		/* Copy away string, but skip control chars. Mainly because
		   Jabber really doesn't like them. */
		for( i = j = 0; cmd[1][i]; i ++ )
			if( ( away[j] = cmd[1][i] ) >= ' ' )
				j ++;
		away[j] = '\0';
		
		irc_send_num( irc, 306, ":You're now away: %s", away );
		set_setstr( &irc->b->set, "away", away );
	}
	else
	{
		irc_send_num( irc, 305, ":Welcome back" );
		set_setstr( &irc->b->set, "away", NULL );
	}
}

static void irc_cmd_version( irc_t *irc, char **cmd )
{
	irc_send_num( irc, 351, "bitlbee-%s. %s :%s/%s ",
	              BITLBEE_VERSION, irc->root->host, ARCH, CPU );
}

static void irc_cmd_completions( irc_t *irc, char **cmd )
{
	help_t *h;
	set_t *s;
	int i;
	
	irc_send_msg_raw( irc->root, "NOTICE", irc->user->nick, "COMPLETIONS OK" );
	
	for( i = 0; commands[i].command; i ++ )
		irc_send_msg_f( irc->root, "NOTICE", irc->user->nick, "COMPLETIONS %s", commands[i].command );
	
	for( h = global.help; h; h = h->next )
		irc_send_msg_f( irc->root, "NOTICE", irc->user->nick, "COMPLETIONS help %s", h->title );
	
	for( s = irc->b->set; s; s = s->next )
		irc_send_msg_f( irc->root, "NOTICE", irc->user->nick, "COMPLETIONS set %s", s->key );
	
	irc_send_msg_raw( irc->root, "NOTICE", irc->user->nick, "COMPLETIONS END" );
}

static void irc_cmd_rehash( irc_t *irc, char **cmd )
{
	if( global.conf->runmode == RUNMODE_INETD )
		ipc_master_cmd_rehash( NULL, NULL );
	else
		ipc_to_master( cmd );
	
	irc_send_num( irc, 382, "%s :Rehashing", global.conf_file );
}

static const command_t irc_commands[] = {
	{ "pass",        1, irc_cmd_pass,        0 },
	{ "user",        4, irc_cmd_user,        IRC_CMD_PRE_LOGIN },
	{ "nick",        1, irc_cmd_nick,        0 },
	{ "quit",        0, irc_cmd_quit,        0 },
	{ "ping",        0, irc_cmd_ping,        0 },
	{ "pong",        0, irc_cmd_pong,        IRC_CMD_LOGGED_IN },
	{ "join",        1, irc_cmd_join,        IRC_CMD_LOGGED_IN },
	{ "names",       1, irc_cmd_names,       IRC_CMD_LOGGED_IN },
	{ "part",        1, irc_cmd_part,        IRC_CMD_LOGGED_IN },
	{ "whois",       1, irc_cmd_whois,       IRC_CMD_LOGGED_IN },
	{ "whowas",      1, irc_cmd_whowas,      IRC_CMD_LOGGED_IN },
	{ "motd",        0, irc_cmd_motd,        IRC_CMD_LOGGED_IN },
	{ "mode",        1, irc_cmd_mode,        IRC_CMD_LOGGED_IN },
	{ "who",         0, irc_cmd_who,         IRC_CMD_LOGGED_IN },
	{ "privmsg",     1, irc_cmd_privmsg,     IRC_CMD_LOGGED_IN },
	{ "nickserv",    1, irc_cmd_nickserv,    IRC_CMD_LOGGED_IN },
	{ "ns",          1, irc_cmd_nickserv,    IRC_CMD_LOGGED_IN },
	{ "away",        0, irc_cmd_away,        IRC_CMD_LOGGED_IN },
	{ "version",     0, irc_cmd_version,     IRC_CMD_LOGGED_IN },
	{ "completions", 0, irc_cmd_completions, IRC_CMD_LOGGED_IN },
	{ "userhost",    1, irc_cmd_userhost,    IRC_CMD_LOGGED_IN },
	{ "ison",        1, irc_cmd_ison,        IRC_CMD_LOGGED_IN },
	{ "watch",       1, irc_cmd_watch,       IRC_CMD_LOGGED_IN },
	{ "invite",      2, irc_cmd_invite,      IRC_CMD_LOGGED_IN },
#if 0
	{ "notice",      1, irc_cmd_privmsg,     IRC_CMD_LOGGED_IN },
#endif
	{ "topic",       1, irc_cmd_topic,       IRC_CMD_LOGGED_IN },
	{ "oper",        2, irc_cmd_oper,        IRC_CMD_LOGGED_IN },
	{ "die",         0, NULL,                IRC_CMD_OPER_ONLY | IRC_CMD_TO_MASTER },
	{ "deaf",        0, NULL,                IRC_CMD_OPER_ONLY | IRC_CMD_TO_MASTER },
	{ "wallops",     1, NULL,                IRC_CMD_OPER_ONLY | IRC_CMD_TO_MASTER },
	{ "wall",        1, NULL,                IRC_CMD_OPER_ONLY | IRC_CMD_TO_MASTER },
	{ "rehash",      0, irc_cmd_rehash,      IRC_CMD_OPER_ONLY },
	{ "restart",     0, NULL,                IRC_CMD_OPER_ONLY | IRC_CMD_TO_MASTER },
	{ "kill",        2, NULL,                IRC_CMD_OPER_ONLY | IRC_CMD_TO_MASTER },
	{ NULL }
};

void irc_exec( irc_t *irc, char *cmd[] )
{	
	int i, n_arg;
	
	if( !cmd[0] )
		return;
	
	for( i = 0; irc_commands[i].command; i++ )
		if( g_strcasecmp( irc_commands[i].command, cmd[0] ) == 0 )
		{
			/* There should be no typo in the next line: */
			for( n_arg = 0; cmd[n_arg]; n_arg ++ ); n_arg --;
			
			if( irc_commands[i].flags & IRC_CMD_PRE_LOGIN && irc->status & USTATUS_LOGGED_IN )
			{
				irc_send_num( irc, 462, ":Only allowed before logging in" );
			}
			else if( irc_commands[i].flags & IRC_CMD_LOGGED_IN && !( irc->status & USTATUS_LOGGED_IN ) )
			{
				irc_send_num( irc, 451, ":Register first" );
			}
			else if( irc_commands[i].flags & IRC_CMD_OPER_ONLY && !strchr( irc->umode, 'o' ) )
			{
				irc_send_num( irc, 481, ":Permission denied - You're not an IRC operator" );
			}
			else if( n_arg < irc_commands[i].required_parameters )
			{
				irc_send_num( irc, 461, "%s :Need more parameters", cmd[0] );
			}
			else if( irc_commands[i].flags & IRC_CMD_TO_MASTER )
			{
				/* IPC doesn't make sense in inetd mode,
				    but the function will catch that. */
				ipc_to_master( cmd );
			}
			else
			{
				irc_commands[i].execute( irc, cmd );
			}
			
			return;
		}
	
	if( irc->status >= USTATUS_LOGGED_IN )
		irc_send_num( irc, 421, "%s :Unknown command", cmd[0] );
}
