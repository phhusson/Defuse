/*
 * Build with :
 * gcc defuse.c -o defuse -ldb -Wall -g `pkg-config fuse --cflags --libs` ezxml.c
 * Use with a despotify gateway running on localhost:1234
 * Search for the SETTING string to know what's tunnable
 * Please note that this works only single threaded, so add -s flag when running.
 */
#define _GNU_SOURCE
#define FUSE_USE_VERSION 26
//SETTING:Uncomment this to drop asserts and log messages
//#define NDEBUG
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include "ezxml.h"

void write2(int fd, const char *str) {
	write(fd, str, strlen(str));
}
#define CACHE_PATH "/tmp/despotify-cache"

#define BUF_SZ 256*1024 

static int fd;
pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;

//Warning: The id/key's sizes are currently fixed
//but despotify's documentation says it may change.
typedef struct {
	char *name;
	char id[35];
} PL;

PL *pls;
pthread_mutex_t pls_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	char *name;
	char pl_id[35];
	char sg_id[33];
	char key[33];
	long long int size;
	ezxml_t tree;
	int end;
	pthread_mutex_t mutex;
} SONG;
SONG *sgs;
int sg_pos;
pthread_mutex_t sgs_mutex = PTHREAD_MUTEX_INITIALIZER;

//SETTING
//Force to use only 160kbps files
//(may not work, see get_substream)
static int only_160=0;

//General communication protocol with despotify gateway
char *get_answer(int *ln) {
	char *buffer=malloc(BUF_SZ);
	int ret,tot,tot2;
	memset(buffer, 0, BUF_SZ);
	tot=read(fd, buffer, BUF_SZ);
	char msg[256];
	sscanf(buffer, "%d %d %255s\n", &ret, &tot2, msg);
	*ln=tot2;
	if(ret!=200) {
		fprintf(stderr, "Spotify GW ERROR: %s\n", buffer);
		exit(1);
	}
	char *pos=index(buffer, '\n');
	if(pos) {
		long long int n=pos-buffer+1;
		//memmove(buffer, buffer+n, tot-n);
		tot=tot-n;
		int i;
		for(i=0;i<tot;++i)
			buffer[i]=buffer[i+n];
	} else
		tot=0;
	buffer=realloc(buffer, tot2+2);
	while(tot<tot2) {
		ret=read(fd, buffer+tot, tot2-tot);
		tot+=ret;
	}
	assert(tot==tot2);
	return buffer;
}

int get_answer2(char *buf, int ln) {
	char *buffer=malloc(BUF_SZ);
	memset(buffer, 0, BUF_SZ);
	int ret,tot,tot2;
	tot=read(fd, buffer, BUF_SZ);
	char msg[256];
	sscanf(buffer, "%d %d %255s\n", &ret, &tot2, msg);
	if(ret!=200) {
		fprintf(stderr, "Spotify GW ERROR: %s\n", buffer);
		exit(1);
	}

	if(tot2>ln)
		tot2=ln;
	char *pos=index(buffer, '\n');
	if(pos) {
		long long int n=pos-buffer+1;
		memcpy(buf, buffer+n, tot-n);
		tot=tot-n;
	} else
		tot=0;
	free(buffer);
	while(tot<tot2) {
		ret=read(fd, buf+tot, tot2-tot);
		tot+=ret;
	}
	printf("tot2=%d\n", tot2);
	assert(tot==tot2);
	return tot2;
}

//Create connection and login
int init(const char *host, const char *port, const char *user, const char *pass) {
	pls=NULL;sgs=NULL;
	if(!user || !pass) {
		fprintf(stderr, "Won't login without login/pass\n");
		return -1;
	}
	{ 
		mkdir(CACHE_PATH, 0777);
		mkdir(CACHE_PATH"/songs", 0777);
		mkdir(CACHE_PATH"/playlists", 0777);
		mkdir(CACHE_PATH"/files", 0777);
		errno=0;//I'm lazy today. Will clean later (TODO)
		if(!host)
			host="localhost";
		if(!port)
			port="1234";

		int err;
		struct addrinfo hints, *res0, *res;


		//hints is a structure like the result, but with some undefined values,
		//defined ones wouldn't be remplaced
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = PF_UNSPEC;  
		hints.ai_socktype = SOCK_STREAM;


		/* Obtain AAAA and A record using getaddrinfo*/
		err = getaddrinfo(host, port, &hints, &res0);

		if (err) {
			fprintf(stderr, "error : %s", gai_strerror(err));
			freeaddrinfo(res0);
			return -1;
		}


		/* Use the result of getaddrinfo, and try every result until connection is established */
		for (res = res0; res; res = res->ai_next) {  
			fd = socket (res->ai_family, res->ai_socktype, res->ai_protocol);  
			if (fd < 0)  
				continue;  
			if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {  
				close (fd);  
				continue;
			}
			break;
		}
		freeaddrinfo(res0);
	}

	char *cmd=NULL;

	asprintf(&cmd, "login %s %s\n", user, pass);
	int ln;
	char *ret;

	//Should be useless here
	pthread_mutex_lock(&fd_mutex);
	{
		write2(fd, cmd);
		ret=get_answer(&ln);
	}
	pthread_mutex_unlock(&fd_mutex);

	free(cmd);
	free(ret);
	assert(ln==0);
	return 0;
}

//Download xml description file of the track
//the force flag will redownload it even if it has already been donwloaded
//Should be useless most time,
//But still you should do some rm -Rf CACHE_PATH/songs/ for the 320kbps migration
//Or see the appropriate setting
pthread_mutex_t cache_tracks_mutex = PTHREAD_MUTEX_INITIALIZER;
void fill_track(const char *id, int force) {
	char *filename;
	int r;
	asprintf(&filename, CACHE_PATH "/songs/%s.xml", id);
	//We should set a lock on a per file basis
	//But it's not easy to code, so do it globally
	pthread_mutex_lock(&cache_tracks_mutex);
	if(force || (r=access(filename, R_OK))!=0) {

		if(!id)
			return;
		char *cmd,*ret;
		asprintf(&cmd, "browsetrack %s\n", id);
		pthread_mutex_lock(&fd_mutex);
		{
			write2(fd, cmd);
			ret=get_answer(&r);
		}
		pthread_mutex_unlock(&fd_mutex);

		free(cmd);cmd=NULL;
		if(force) {
			unlink(filename);
			errno=0;
		}
		int f=open(filename, O_CREAT|O_WRONLY|O_TRUNC, 0666);
		write(f, ret, r);
		free(ret);
	}
	pthread_mutex_unlock(&cache_tracks_mutex);
	free(filename);
}

//Return an ezxml tree of the song
ezxml_t get_track(const char *id) {
	//SETTING:
	//You may set 1 here (force flag of fill_track), so you won't have any delay with the 320kbps migration
	//But sometimes this takes a lot of time (or 500 GW error)
	fill_track(id, 0);
	char *filename;
	asprintf(&filename, CACHE_PATH "/songs/%s.xml", id);
	pthread_mutex_lock(&cache_tracks_mutex);
	ezxml_t tree;
	{
		tree=ezxml_parse_file(filename);
	}
	pthread_mutex_unlock(&cache_tracks_mutex);
	free(filename);
	return tree;
}

//Download xml description file of the playlist
//Mainly contains song ids and playlist name
//Also for 0^34 ("playlist" of playlists)
//You should force update at every update of the playlist...
//For little playlists you can force it everytime, but for big ones you'd better not
pthread_mutex_t cache_pl_mutex = PTHREAD_MUTEX_INITIALIZER;
void fill_pl(const char *id, int force) {
	char *filename;
	int r;
	if(!id)
		return;
	asprintf(&filename, CACHE_PATH "/playlists/%s.xml", id);
	//We should set a lock on a per file basis
	//But it's not easy to code, so do it globally
	pthread_mutex_lock(&cache_pl_mutex);
	if(force || (r=access(filename, R_OK))!=0) {

		char *cmd,*ret;
		asprintf(&cmd, "playlist %s\n", id);
		pthread_mutex_lock(&fd_mutex);
		{
			write2(fd, cmd);
			ret=get_answer(&r);
		}
		pthread_mutex_unlock(&fd_mutex);
		free(cmd);cmd=NULL;
		if(force) {
			unlink(filename);
			errno=0;
		}
		int f=open(filename, O_CREAT|O_WRONLY|O_TRUNC, 0666);
		write(f, ret, r);
		free(ret);
	}
	pthread_mutex_unlock(&cache_pl_mutex);
	free(filename);
}

//Return an ezxml tree of the playlist
ezxml_t get_pl(const char *id) {
	//SETTING:
	//If you use little playlists (<30), you could set 1 here (force flag of fill_pl)
	//This will get you realtime (meaning at every restart of defuse), playlist update
	//But for big playlists, this may be wwwaaaaayyyyyy too slow
	//(sometimes my 300+ playlist takes more than GW timeout...)
	fill_pl(id, 0);
	char *filename;
	asprintf(&filename, CACHE_PATH "/playlists/%s.xml", id);
	ezxml_t tree;
	pthread_mutex_lock(&cache_pl_mutex);
	{
		tree=ezxml_parse_file(filename);
	}
	pthread_mutex_unlock(&cache_pl_mutex);
	free(filename);
	return tree;
}

//Return the key for a song/file pair
char *get_key(char *sg_id, char *fileid) {
	char *cmd,*answer;
	int ln=0;
	asprintf(&cmd, "key %s %s\n", fileid, sg_id);
	pthread_mutex_lock(&fd_mutex);
	{
		write2(fd, cmd);
		answer=get_answer(&ln);
	}
	pthread_mutex_unlock(&fd_mutex);
	free(cmd);
#ifndef NDEBUG
	fprintf(stderr, "Got key !\n");
#endif
	char *ret;

	{
		int i;
		for(i=0;i<ln;++i)
			if(answer[i]==0)
				answer[i]=' ';
	}
#ifndef NDEBUG
	fprintf(stderr, "ln=%d\n", ln);
	write(2, answer, ln);
#endif
	ezxml_t tree=ezxml_parse_str(answer, ln);
	ezxml_t key=ezxml_get(tree, "key", -1);
	ret=strdup(ezxml_txt(key));
	ezxml_free(tree);
	free(answer);
#ifndef NDEBUG
	fprintf(stderr, "Key is %s!\n", ret);
#endif
	return ret;
}

//Explicit
char *get_playlist_name(const char *id) {
	ezxml_t tree=get_pl(id);
	//The XML Tree isn't explicit at all...
	//Look at CACHE_PATH/playlists/*.xml if you want to check/understand this
	char *ret=strdup(ezxml_txt(ezxml_get(tree, "next-change", 0, "change", 0, "ops", 0, "name", -1)));
	ezxml_free(tree);
	return ret;
}

char *get_song_name(SONG sg) {
	//SETTING
	//Change here if you want the files to have
	//names other than "artist - album - title.ogg"
	char *artist=ezxml_txt(ezxml_get(sg.tree, "tracks", 0, "track", 0, "artist", -1));
	char *title=ezxml_txt(ezxml_get(sg.tree, "tracks", 0, "track", 0, "title", -1));
	char *album=ezxml_txt(ezxml_get(sg.tree, "tracks", 0, "track", 0, "album", -1));
	char *ret;
	asprintf(&ret, "%s - %s - %s.ogg", artist, album, title);
	return ret;
}

int get_substream(SONG *sg, long long int offset, int length, char *buf) {
	//TODO Cache !
	//But needs sparse files, and can't find any easy way to do it
	ezxml_t file=ezxml_get(sg->tree, "tracks", 0, "track", 0, "files", 0, "file", !only_160, "");
	//Assume 320kbps are in second row (proven right until now)
	//TODO: do it a better way, but needs string parsing (with a string i don't totally understand...)
	if(!file)
		file=ezxml_get(sg->tree, "tracks", 0, "track", 0, "files", 0, "file", 0, "");
	assert(file);
	char fileid[41];
	strncpy(fileid, ezxml_attr(file, "id"), 41);
	fileid[40]=0;

	//Caching fileid key
	//TODO: Is the key constant for a file ?
	//Else we will have to manage that problem (only one global key which will be destroyed at every file change?)
	pthread_mutex_lock(&(sg->mutex));
	if(sg->key[0]==0) {
		char *key=get_key(sg->pl_id, fileid);
		strncpy(sg->key, key, 33);
		free(key);
		sg->key[32]=0;
	}
	pthread_mutex_unlock(&(sg->mutex));

	char *cmd;
	int ret;
	asprintf(&cmd, "substream %s %lld %d %s\n", fileid, offset, length, sg->key);
	pthread_mutex_lock(&fd_mutex);
	{
		write2(fd, cmd);
		ret=get_answer2(buf, length);
	}
	pthread_mutex_unlock(&fd_mutex);
	free(cmd);
	if(offset==0) {
		//Please note that this 167 hack is needed but still not undertstood:
		//There is some Ogg-alike garbage at the start of the stream
		if(ret<=167)
			//Couldn't get anything useful...
			//Happens sometime, still don't know why.
			return 0;
		ret-=167;
		memmove(buf, buf+167, ret);
		return ret;
	} else {
		//Should we do a 167 here too ?
		//Not so sure, but it seems to works right (but with a little glitch)
		return ret;
	}
}

long long int get_song_length(SONG sg) {
	ezxml_t file2=ezxml_get(sg.tree, "tracks", 0, "track", 0, "files", 0,"file", 1, "");
	int approx;
	if(file2 && !only_160)
		approx=320000/(8*1000)*atoi(ezxml_txt(ezxml_get(sg.tree, "tracks", 0, "track", 0, "length", -1)));
	else
		approx=160000/(8*1000)*atoi(ezxml_txt(ezxml_get(sg.tree, "tracks", 0, "track", 0, "length", -1)));
	//Here's a better way to do it, but wwwaaaaaaaayyyyyyy slower.
	//Maybe BUF_SZ doesn't really fits the role
	/*
	int start=approx-BUF_SZ/2;
	int size=get_substream(sg, start, BUF_SZ, NULL)+start;
	return size;
	*/
	return approx;
}


void clear_pls() {
	pthread_mutex_lock(&pls_mutex);
	unsigned int i;
	if(!pls)
		return;
	for(i=0;pls[i].name; ++i) {
		if(pls[i].name)
			free(pls[i].name);
	}
	free(pls);
	pls=NULL;
	pthread_mutex_unlock(&pls_mutex);
}

void Check_cache() {
	pthread_mutex_lock(&pls_mutex);
	if(pls==NULL) {
		ezxml_t tree=get_pl("0000000000000000000000000000000000");
		char *items=ezxml_txt(ezxml_get(tree, "next-change", 0, "change", 0, "ops", 0, "add", 0, "items", -1));
		int pls_pos=0;
		char *cur_pl;
		if(!items) {
			ezxml_free(tree);
			return;
		}
		while(items && items[0]) {
			pls=realloc(pls, (pls_pos+2)*sizeof(PL));
			while(!isxdigit(items[0]))
				items++;
			cur_pl=strndup(items, 35);
			cur_pl[34]=0;
			char *pl_name=get_playlist_name(cur_pl);
			if(!pl_name) { 
				//Ouch...
				fprintf(stderr, "Couldn't retrieve playlist name\n");
				pl_name=strdup("Unknown name");
			}

			PL cur;
			strncpy(cur.id, cur_pl, 35);
			cur.name=pl_name;
			memcpy(&(pls[pls_pos]), &cur, sizeof(PL));
			free(cur_pl);
			pls_pos++;

			items=index(items, ',');
		}
		pls[pls_pos].name=NULL;
		ezxml_free(tree);
	}
	pthread_mutex_unlock(&pls_mutex);
}
char **get_playlists() {
	Check_cache();
	char **playlists=NULL;
	unsigned int i;
	for(i=0;pls[i].name; ++i) {
		playlists=(char**)realloc(playlists, (i+2)*sizeof(char*));
		playlists[i]=strdup(pls[i].name);
	}
	playlists[i]=NULL;
	return playlists;
}


void fillin_sgs(char *plid) {
	ezxml_t tree=get_pl(plid);
	char *items=ezxml_txt(ezxml_get(tree, "next-change", 0, "change", 0, "ops", 0, "add", 0, "items", -1));
	char *cur_sg;
	if(!items) {
		ezxml_free(tree);
		return;
	}
	int j;
	pthread_mutex_lock(&sgs_mutex);
	//First check it has been filled
	if(sgs)
		for(j=0;sgs[j].name;++j)
			if(strncmp(sgs[j].pl_id, plid, 34)==0) {
				//Found one song in our playlist, assume it has been filled.
				ezxml_free(tree);
				pthread_mutex_unlock(&sgs_mutex);
				return;
			}
	while(items && items[0]) {
		//TODO:Maybe move to some cleaner code (with std::vector or something like this)
		sgs=realloc(sgs, (sg_pos+2)*sizeof(SONG));
		while(!isxdigit(items[0]))
			items++;
		cur_sg=strndup(items, 33);
		cur_sg[32]=0;
		ezxml_t sg_tree=get_track(cur_sg);

		SONG cur;
		strncpy(cur.pl_id, plid, 35);
		strncpy(cur.sg_id, cur_sg, 33);
		cur.key[0]=0;
		cur.tree=sg_tree;
		cur.size=get_song_length(cur);
		cur.end=1000*1000*1000;//A song of 1GB should be fair enough.
		pthread_mutex_init(&(cur.mutex), 0);
		{
			char *sg_name=get_song_name(cur);
			if(!sg_name) { 
				//Ouch...
				fprintf(stderr, "Couldn't retrieve playlist name\n");
				sg_name=strdup("Unknown name");
			}
			char *pos=NULL;
			while((pos=index(sg_name, '/'))!=NULL) *pos='\\';
			cur.name=sg_name;
		}

		memcpy(&(sgs[sg_pos]), &cur, sizeof(SONG));
		free(cur_sg);
		sg_pos++;

		items=index(items, ',');
	}
	sgs[sg_pos].name=NULL;
	ezxml_free(tree);
	pthread_mutex_unlock(&sgs_mutex);
}



static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	(void)offset;
	(void)fi;
	if(strcmp(path, "/")==0) {
		//We are in the root, so show playlists
		//And maybe later, searches (TODO)
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		char **playlists=get_playlists();
		if(!playlists)
			return 0;
		int i;
		for(i=0;playlists[i];++i) {
			struct stat stbuf;
			stbuf.st_mode=S_IFDIR|0755;
			stbuf.st_nlink=2;//Every folder has 2 links: himself in his parent, and . in himself

			filler(buf, playlists[i], &stbuf, 0);
			free(playlists[i]);
		}
		free(playlists);
		return 0;
	} else {
		Check_cache();
		unsigned int i;
		for(i=0;pls[i].name; ++i) 
			if(strcmp(pls[i].name, path+1)==0) {
				filler(buf, ".", NULL, 0);
				filler(buf, "..", NULL, 0);
				fillin_sgs(pls[i].id);
				int j;
				pthread_mutex_lock(&sgs_mutex);
				if(sgs)
					for(j=0;sgs[j].name;++j)
						if(strncmp(sgs[j].pl_id, pls[i].id, 34)==0)
							filler(buf, sgs[j].name, NULL, 0);
				pthread_mutex_unlock(&sgs_mutex);

				return 0;
			}
		return -ENOENT;
	}
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	int j;
	char *path2=strdup(path);
	char *filename=index(path2+1, '/');
	char *pl=path2+1;
	filename[0]=0;
	filename++;
	
	//Fill the songs cache for this playlist
	//Usefull only if file accessed directly without folder listing
	for(j=0;pls[j].name;++j)
		if(strncmp(pls[j].name, pl ,34)==0)
			fillin_sgs(pls[j].id);
	if(!sgs) {
		free(path2);
		return -ENOENT;
	}
	SONG *cur;
	cur=NULL;
	pthread_mutex_lock(&sgs_mutex);
	for(j=0;sgs[j].name;++j) {
		if(strcmp(sgs[j].name, filename)==0) {
			int i;
			for(i=0;pls[i].name;++i) {
				if(strcmp(pls[i].name, pl)==0) {
					cur=&sgs[j];
					break;
				}
			}
		}
	}
	pthread_mutex_unlock(&sgs_mutex);
	free(path2);
	if(cur==NULL) 
		return -ENOENT;
	if(offset>cur->end ) {
		memset(buf, 0, size);
		return size;
	}

	int ret=get_substream(cur, offset, size, buf);
	if(ret<size && offset!=0)
		cur->end=offset+ret;
	return get_substream(cur, offset, size, buf);
}

static int fs_getattr(const char *path, struct stat *stbuf) {
	memset(stbuf, 0, sizeof(struct stat));
	if(strcmp(path, "/")==0) {
		stbuf->st_mode=S_IFDIR|0755;
		stbuf->st_nlink=2;
		return 0;
	} else if(index(path+1, '/')==NULL) {
		int got=0;
		char **playlists=get_playlists();
		if(!playlists)
			return -ENOENT;
		int i;
		for(i=0;playlists[i];++i) {
			if(strcmp(playlists[i], path+1)==0)
				got++;
			free(playlists[i]);
		}
		free(playlists);
		if(got==0)
			//The node asked isn't a playlist
			//So it doesn't exist.
			return -ENOENT;
		else {
			//It is a playlist
			//So it does exist.
			stbuf->st_mode=S_IFDIR|0755;
			stbuf->st_nlink=2;
			return 0;
		}
	} else {
		//We are in a folder
		int j;
		char *path2=strdup(path);
		char *filename=index(path2+1, '/');
		char *pl=path2+1;
		filename[0]=0;
		filename++;
		if(index(filename, '/'))
			//We are in a sub-folder... It shouldn't happen.
			return -ENOENT;
		pthread_mutex_lock(&pls_mutex);
		for(j=0;pls[j].name;++j)
			if(strncmp(pls[j].name, pl ,34)==0)
				fillin_sgs(pls[j].id);
		pthread_mutex_unlock(&pls_mutex);
		if(!sgs) {
			//After filling song cache there is no song...
			//Just give up
			free(path2);
			return -ENOENT;
			//Maybe exit()?
		}
		pthread_mutex_lock(&sgs_mutex);
		for(j=0;sgs[j].name;++j) {
			if(strcmp(sgs[j].name, filename)==0) {
				//Name matches
				int i;
				pthread_mutex_lock(&pls_mutex);
				for(i=0;pls[i].name;++i) {
					if(strcmp(pls[i].name, pl)==0) {
						//And playlist name too !
						stbuf->st_mode=S_IFREG|0666;
						stbuf->st_nlink=1;
						stbuf->st_size=sgs[j].size;
						free(path2);
						pthread_mutex_unlock(&pls_mutex);
						pthread_mutex_unlock(&sgs_mutex);
						return 0;
					}
				}
				pthread_mutex_unlock(&pls_mutex);
			}
		}
		pthread_mutex_unlock(&sgs_mutex);
		free(path2);
		return -ENOENT;
	}

}

static struct fuse_operations fs_oper;

int main(int argc, char **argv) {
	char *user=getenv("DESPOT_USER");
	char *pass=getenv("DESPOT_PASS");

	if(init(NULL, NULL, user, pass)) {
		fprintf(stderr, "FAILED\n");
		return -1;
	}
	fs_oper.getattr=fs_getattr;
	fs_oper.readdir=fs_readdir;
	fs_oper.read=fs_read;
	Check_cache();

	fuse_main(argc, argv, &fs_oper, NULL);
	return 0;
}
