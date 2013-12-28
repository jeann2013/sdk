/**
 * @file sync.cpp
 * @brief Class for synchronizing local and remote trees
 *
 * (c) 2013 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/sync.h"
#include "mega/megaapp.h"
#include "mega/transfer.h"

namespace mega {

// new Syncs are automatically inserted into the session's syncs list
// a full read of the subtree is initiated
Sync::Sync(MegaClient* cclient, string* crootpath, Node* remotenode, int ctag)
{
	client = cclient;
	tag = ctag;

	localbytes = 0;
	localnodes[FILENODE] = 0;
	localnodes[FOLDERNODE] = 0;

	state = SYNC_INITIALSCAN;

	dirnotify = client->fsaccess->newdirnotify(crootpath);

	localroot.init(this,FOLDERNODE,NULL,crootpath,crootpath);
	localroot.setnode(remotenode);

	sync_it = client->syncs.insert(client->syncs.end(),this);
}

Sync::~Sync()
{
	// prevent remote mass deletion while rootlocal destructor runs
	state = SYNC_CANCELED;

	client->syncs.erase(sync_it);

	client->syncactivity = true;
}

void Sync::changestate(syncstate newstate)
{
	if (newstate != state)
	{
		client->app->syncupdate_state(this,newstate);

		state = newstate;
	}
}

// walk path and return corresponding LocalNode and its parent
// path must be relative to l or start with the root prefix if l == NULL
// path must be a full sync path, i.e. start with localroot->localname
// NULL: no match, optionally returns residual path
LocalNode* Sync::localnodebypath(LocalNode* l, string* localpath, LocalNode** parent, string* rpath)
{
	const char* ptr = localpath->data();
	const char* end = ptr+localpath->size();
	size_t separatorlen = client->fsaccess->localseparator.size();

	if (rpath) assert(!rpath->size());

	if (!l)
	{
		// verify matching localroot prefix - this should always succeed for internal use
		if (memcmp(ptr,localroot.localname.data(),localroot.localname.size()) || memcmp(ptr+localroot.localname.size(),client->fsaccess->localseparator.data(),separatorlen))
		{
			if (parent) *parent = NULL;
			return NULL;
		}

		l = &localroot;
		ptr += l->localname.size()+client->fsaccess->localseparator.size();
	}
	
	const char* nptr = ptr;
	localnode_map::iterator it;
	string t;
	
	for (;;)
	{
		if (nptr == end || !memcmp(nptr,client->fsaccess->localseparator.data(),separatorlen))
		{
			if (parent) *parent = l;

			t.assign(ptr,nptr-ptr);
			if ((it = l->children.find(&t)) == l->children.end() && (it = l->schildren.find(&t)) == l->schildren.end())
			{
				// no full match: store residual path, return NULL with the matching component LocalNode in parent
				if (rpath) rpath->assign(ptr,localpath->data()-ptr+localpath->size());
				return NULL;
			}

			l = it->second;

			if (nptr == end)
			{
				// full match: no residual path, return corresponding LocalNode
				if (rpath) rpath->clear();
				return l;
			}

			ptr = nptr+separatorlen;
			nptr = ptr;
		}
		else nptr += separatorlen;
	}
}

// determine sync state of path (path must start with the sync prefix)
pathstate_t Sync::pathstate(string* localpath)
{
	LocalNode* l = localnodebypath(NULL,localpath);

	if (!l) return PATHSTATE_NOTFOUND;
	if (l->node) return PATHSTATE_SYNCED;
	if (l->transfer && l->transfer->slot) return PATHSTATE_SYNCING;
	return PATHSTATE_PENDING;
}

// scan localpath, add or update child nodes, call recursively for folder nodes
// localpath must be prefixed with Sync
bool Sync::scan(string* localpath, FileAccess* fa)
{
	DirAccess* da;
	string localname, name;
	size_t baselen;
	bool success;

	baselen = localroot.localname.size()+client->fsaccess->localseparator.size();
	
	if (baselen > localpath->size()) baselen = localpath->size();

	da = client->fsaccess->newdiraccess();

	// scan the dir, mark all items with a unique identifier
	if ((success = da->dopen(localpath,fa,false)))
	{
		size_t t = localpath->size();

		while (da->dnext(&localname))
		{
			name = localname;
			client->fsaccess->local2name(&name);

			// check if this record is to be ignored
			if (client->app->sync_syncable(name.c_str(),localpath,&localname))
			{
				if (t) localpath->append(client->fsaccess->localseparator);
				localpath->append(localname);

				// new or existing record: place scan result in notification queue
				dirnotify->notify(DirNotify::DIREVENTS,NULL,localpath->data(),localpath->size());
				
				localpath->resize(t);
			}
		}
	}

	delete da;
	
	return success;
}

// check local path - if !localname, localpath is relative to l, with l == NULL being the root of the sync
// if localname is set, localpath is absolute and localname its last component
// path references a new FOLDERNODE: returns created node
// path references a existing FILENODE: returns node
// otherwise, returns NULL
LocalNode* Sync::checkpath(LocalNode* l, string* localpath, string* localname)
{
	LocalNode* ll = l;
	FileAccess* fa;
	bool newnode = false, changed = false;
	bool isroot;

	LocalNode* parent;
	string path;		// UTF-8 representation of tmppath
	string tmppath;		// full path represented by l + localpath
	string newname;		// portion of tmppath not covered by the existing LocalNode structure (always the last path component that does not have a corresponding LocalNode yet)

	if (localname)
	{
		// shortcut case (from within syncdown())
		isroot = false;
		parent = l;
		l = NULL;
		
		client->fsaccess->local2path(localpath,&path);
	}
	else
	{
		// construct full filesystem path in tmppath
		if (l) l->getlocalpath(&tmppath);

		if (localpath->size())
		{
			if (tmppath.size()) tmppath.append(client->fsaccess->localseparator);
			tmppath.append(*localpath);
		}

		// look up deepest existing LocalNode by path, store remainder (if any) in newname
		l = localnodebypath(l,localpath,&parent,&newname);

		// path invalid?
		if (!l && !newname.size()) return NULL;

		string name = newname;
		client->fsaccess->local2name(&name);
		if (!client->app->sync_syncable(name.c_str(),&tmppath,&newname)) return NULL;

		isroot = l == &localroot && !newname.size();

		client->fsaccess->local2path(&tmppath,&path);
	}

	// attempt to open/type this file
	fa = client->fsaccess->newfileaccess();

	if (fa->fopen(localname ? localpath : &tmppath,true,false))
	{
		if (!isroot)
		{
			if (l)
			{
				// has the file been overwritten since the last scan?
				// (we tolerate overwritten folders, because we do a content scan anyway)
				if (fa->type == FILENODE && (fa->fsidvalid && l->fsid_it != client->fsidnode.end() && l->fsid != fa->fsid))
				{
					l->setnotseen(l->notseen+1);
					l = NULL;
				}
				else
				{
					if (fa->fsidvalid) l->setfsid(fa->fsid);
					l->setnotseen(0);
					l->scanseqno = scanseqno;
				}
			}

			// new node
			if (!l)
			{
				// rename or move of existing node?
				handlelocalnode_map::iterator it;

				if (fa->fsidvalid && (it = client->fsidnode.find(fa->fsid)) != client->fsidnode.end())
				{
					client->app->syncupdate_local_move(this,it->second->name.c_str(),path.c_str());

					// (in case of a move, this synchronously updates l->parent and l->node->parent)
					it->second->setnameparent(parent,localname ? localpath : &tmppath);

					// unmark possible deletion
					it->second->setnotseen(0);
				}
				else
				{
					// this is a new node: add
					l = new LocalNode;
					l->init(this,fa->type,parent,localname ? localname : &newname,localname ? localpath : &tmppath);
					if (fa->fsidvalid) l->setfsid(fa->fsid);
					newnode = true;
				}
			}
		}

		if (l)
		{
			// detect file changes or recurse into new subfolders
			if (l->type == FOLDERNODE)
			{
				if (newnode)
				{
					scan(localname ? localpath : &tmppath,fa);
					client->app->syncupdate_local_folder_addition(this,path.c_str());
				}
				else l = NULL;
			}
			else
			{
				if (isroot) changestate(SYNC_FAILED);	// root node cannot be a file
				else
				{
					if (l->size > 0) localbytes -= l->size;
					if (l->genfingerprint(fa)) changed = true;
					if (l->size > 0) localbytes += l->size;
					
					if (newnode) client->app->syncupdate_local_file_addition(this,path.c_str());
					else if (changed) client->app->syncupdate_local_file_change(this,path.c_str());
				}
			}
		}

		if (changed || newnode)
		{

			client->syncadded.insert(l->syncid);
			client->syncactivity = true;
		}
	}
	else
	{
		if (fa->retry)
		{
			// fopen() signals that the failure is potentially transient - do nothing and request a recheck
			dirnotify->notify(DirNotify::RETRY,ll,localpath->data(),localpath->size());
		}
		else if (l)
		{
			// immediately stop outgoing transfer, if any
			if (l->transfer) client->stopxfer(l);
			
			client->syncactivity = true;
			l->setnotseen(1);
		}
		
		l = NULL;
	}

	delete fa;
	
	return l;
}

// add or refresh local filesystem item from scan stack, add items to scan stack
void Sync::procscanq(int q)
{
	while (dirnotify->notifyq[q].size())
	{
		LocalNode* l = checkpath(dirnotify->notifyq[q].front().localnode,&dirnotify->notifyq[q].front().path);
		dirnotify->notifyq[q].pop_front();
		
		// we return control to the application in case a filenode was added
		// (in order to avoid lengthy blocking episodes due to multiple consecutive fingerprint calculations)
		if (l && l->type == FILENODE) break;
	}

	if (dirnotify->notifyq[q].size()) client->syncactivity = true;
	else if (!dirnotify->notifyq[!q].size()) scanseqno++;	// all queues empty: new scan sweep begins
}

} // namespace