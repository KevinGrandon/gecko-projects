/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is nsMemoryCacheDevice.cpp, released February 22, 2001.
 * 
 * The Initial Developer of the Original Code is Netscape Communications
 * Corporation.  Portions created by Netscape are
 * Copyright (C) 2001 Netscape Communications Corporation.  All
 * Rights Reserved.
 * 
 * Contributor(s): 
 *    Gordon Sheridan <gordon@netscape.com>
 *    Patrick C. Beard <beard@netscape.com>
 */

#include "nsDiskCacheDevice.h"
#include "nsICacheService.h"
#include "nsIFileTransportService.h"
#include "nsDirectoryServiceDefs.h"
#include "nsISupportsArray.h"
#include "nsXPIDLString.h"
#include "nsIInputStream.h"
#include "nsIOutputStream.h"

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

// I don't want to have to use preferences to obtain this, rather, I would
// rather be initialized with this information. To get started, the same
// mechanism

#include "nsIPref.h"

static const char CACHE_DIR_PREF[] = { "browser.cache.directory" };

static int PR_CALLBACK cacheDirectoryChanged(const char *pref, void *closure)
{
	nsresult rv;
	NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_CONTRACTID, &rv);
	if (NS_FAILED(rv))
		return rv;
    
	nsCOMPtr<nsILocalFile> cacheDirectory;
    rv = prefs->GetFileXPref(CACHE_DIR_PREF, getter_AddRefs( cacheDirectory ));
	if (NS_FAILED(rv))
		return rv;

    nsDiskCacheDevice* device = NS_STATIC_CAST(nsDiskCacheDevice*, closure);
    device->setCacheDirectory(cacheDirectory);
    
    return NS_OK;
}

static nsresult installPrefListeners(nsDiskCacheDevice* device)
{
	nsresult rv;
	NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_CONTRACTID, &rv);
	if (NS_FAILED(rv))
		return rv;
	rv = prefs->RegisterCallback(CACHE_DIR_PREF, cacheDirectoryChanged, device); 
	if (NS_FAILED(rv))
		return rv;

	nsCOMPtr<nsILocalFile> cacheDirectory;
    rv = prefs->GetFileXPref(CACHE_DIR_PREF, getter_AddRefs(cacheDirectory));
    if (NS_FAILED(rv)) {
#if DEBUG
        // XXX use current process directory during development only.
        nsCOMPtr<nsIFile> currentProcessDir;
        rv = NS_GetSpecialDirectory(NS_XPCOM_CURRENT_PROCESS_DIR, 
                                    getter_AddRefs(currentProcessDir));
    	if (NS_FAILED(rv))
    		return rv;

        cacheDirectory = do_QueryInterface(currentProcessDir, &rv);
    	if (NS_FAILED(rv))
    		return rv;
        rv = cacheDirectory->Append("Cache");
    	if (NS_FAILED(rv))
    		return rv;
        rv = prefs->SetFileXPref(CACHE_DIR_PREF, cacheDirectory);
    	if (NS_FAILED(rv))
    		return rv;
#else
        return rv;
#endif
    } else {
        // cause the preference to be set up initially.
        device->setCacheDirectory(cacheDirectory);
    }
    
    return NS_OK;
}

static nsresult removePrefListeners(nsDiskCacheDevice* device)
{
	nsresult rv;
	NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_CONTRACTID, &rv);
	if ( NS_FAILED (rv ) )
		return rv;

	rv = prefs->UnregisterCallback(CACHE_DIR_PREF, cacheDirectoryChanged, device);
	if ( NS_FAILED( rv ) )
		return rv;

    return NS_OK;
}

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

class DiskCacheEntry : public nsISupports {
public:
    NS_DECL_ISUPPORTS

    DiskCacheEntry()
        :   mDirty(PR_FALSE)
    {
        NS_INIT_ISUPPORTS();
    }

    virtual ~DiskCacheEntry() {}

    /**
     * Maps a cache access mode to a cached nsITransport for that access
     * mode. We keep these cached to avoid repeated trips to the
     * file transport service.
     */
    nsCOMPtr<nsITransport>& getTransport(nsCacheAccessMode mode)
    {
        return mTransports[mode - 1];
    }
    
    nsCOMPtr<nsITransport>& getMetaTransport(nsCacheAccessMode mode)
    {
        return mMetaTransports[mode - 1];
    }
    
    PRBool isDirty()
    {
        return mDirty;
    }
    
    void setDirty(PRBool dirty)
    {
        mDirty = dirty;
    }

private:
    nsCOMPtr<nsITransport> mTransports[3];
    nsCOMPtr<nsITransport> mMetaTransports[3];
    PRBool mDirty;
};
NS_IMPL_ISUPPORTS0(DiskCacheEntry);

static DiskCacheEntry*
getDiskCacheEntry(nsCacheEntry * entry)
{
    nsCOMPtr<nsISupports> data;
    entry->GetData(getter_AddRefs(data));
    return (DiskCacheEntry*) data.get();
}

static DiskCacheEntry*
ensureDiskCacheEntry(nsCacheEntry * entry)
{
    nsCOMPtr<nsISupports> data;
    nsresult rv = entry->GetData(getter_AddRefs(data));
    if (NS_SUCCEEDED(rv) && !data) {
        data = new DiskCacheEntry();
        if (NS_SUCCEEDED(rv) && data)
            entry->SetData(data.get());
    }
    return (DiskCacheEntry*) data.get();
}

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

nsDiskCacheDevice::nsDiskCacheDevice()
    :   mScannedEntries(PR_FALSE), mTotalCachedDataSize(LL_ZERO)
{
}

nsDiskCacheDevice::~nsDiskCacheDevice()
{
    removePrefListeners(this);
}

nsresult
nsDiskCacheDevice::Init()
{
    nsresult rv = installPrefListeners(this);
    if (NS_FAILED(rv)) return rv;
    
    rv = mBoundEntries.Init();
    if (NS_FAILED(rv)) return rv;
    
    return  NS_OK;
}

nsresult
nsDiskCacheDevice::Create(nsCacheDevice **result)
{
    nsDiskCacheDevice * device = new nsDiskCacheDevice();
    if (!device)  return NS_ERROR_OUT_OF_MEMORY;

    nsresult rv = device->Init();
    if (NS_FAILED(rv)) {
        delete device;
        device = nsnull;
    }
    *result = device;
    return rv;
}


const char *
nsDiskCacheDevice::GetDeviceID()
{
    return "disk";
}


nsCacheEntry *
nsDiskCacheDevice::FindEntry(nsCString * key)
{
#if 0
    // XXX eager scanning of entries is probably not necessary.
    if (!mScannedEntries) {
        scanEntries();
        mScannedEntries = PR_TRUE;
    }
#endif

    // XXX look in entry hashtable first, if not found, then look on
    // disk, to see if we have a disk cache entry that maches.
    nsCacheEntry * entry = mBoundEntries.GetEntry(key);
    if (!entry) {
        nsresult rv = readDiskCacheEntry(key, &entry);
        if (NS_FAILED(rv)) return nsnull;
        rv = mBoundEntries.AddEntry(entry);
        if (NS_FAILED(rv)) {
            delete entry;
            return nsnull;
        }
    }

    //** need nsCacheService::CreateEntry();
    entry->MarkActive(); // so we don't evict it
    //** find eviction element and move it to the tail of the queue
    
    return entry;
}


nsresult
nsDiskCacheDevice::DeactivateEntry(nsCacheEntry * entry)
{
    nsCString* key = entry->Key();
    nsCacheEntry * ourEntry = mBoundEntries.GetEntry(key);
    NS_ASSERTION(ourEntry, "DeactivateEntry called for an entry we don't have!");
    if (!ourEntry)
        return NS_ERROR_INVALID_POINTER;

    // commit any changes about this entry to disk.
    updateDiskCacheEntry(entry);

    // XXX eventually, as a performance enhancement, keep entries around for a while before deleting them.
    // XXX right now, to prove correctness, destroy the entries eagerly.
#if 0
    mBoundEntries.RemoveEntry(entry);
    delete entry;
#else
    entry->MarkInactive();
#endif

    return NS_OK;
}


nsresult
nsDiskCacheDevice::BindEntry(nsCacheEntry * entry)
{
    nsresult  rv = mBoundEntries.AddEntry(entry);
    if (NS_FAILED(rv))
        return rv;

    //** add size of entry to memory totals
    return NS_OK;
}

nsresult
nsDiskCacheDevice::DoomEntry(nsCacheEntry * entry)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult
nsDiskCacheDevice::GetTransportForEntry(nsCacheEntry * entry,
                                        nsCacheAccessMode mode, 
                                        nsITransport ** result)
{
    NS_ENSURE_ARG_POINTER(entry);
    NS_ENSURE_ARG_POINTER(result);

    DiskCacheEntry* diskEntry = ensureDiskCacheEntry(entry);
    if (!diskEntry) return NS_ERROR_OUT_OF_MEMORY;

    nsCOMPtr<nsITransport>& transport = diskEntry->getTransport(mode);
    if (transport) {
        NS_ADDREF(*result = transport);
        return NS_OK;
    }
       
    // XXX generate the name of the cache entry from the hash code of its key,
    // modulo the number of files we're willing to keep cached.
    nsCOMPtr<nsIFile> entryFile;
    nsresult rv = getFileForKey(entry->Key()->get(), PR_FALSE, getter_AddRefs(entryFile));
    if (NS_SUCCEEDED(rv)) {
        rv = getTransportForFile(entryFile, mode, getter_AddRefs(transport));
        if (NS_SUCCEEDED(rv))
            NS_ADDREF(*result = transport);
    }
    return rv;
}

/**
 * This routine will get called every time an open descriptor.
 */
nsresult
nsDiskCacheDevice::OnDataSizeChange(nsCacheEntry * entry, PRInt32 deltaSize)
{
    PRInt64 deltaSize64;
    LL_I2L(deltaSize64, deltaSize);
    LL_ADD(mTotalCachedDataSize, mTotalCachedDataSize, deltaSize64);
    DiskCacheEntry* diskEntry = getDiskCacheEntry(entry);
    if (diskEntry) diskEntry->setDirty(PR_TRUE);
    return NS_OK;
}

void nsDiskCacheDevice::setCacheDirectory(nsILocalFile* cacheDirectory)
{
    mCacheDirectory = cacheDirectory;
}

nsresult nsDiskCacheDevice::getFileForKey(const char* key, PRBool meta, nsIFile ** result)
{
    if (mCacheDirectory) {
        nsCOMPtr<nsIFile> entryFile;
        nsresult rv = mCacheDirectory->Clone(getter_AddRefs(entryFile));
    	if (NS_FAILED(rv))
    		return rv;
        // generate the hash code for this entry, and use that as a file name.
        PLDHashNumber hash = ::PL_DHashStringKey(NULL, key);
        char name[32];
        ::sprintf(name, "%08X%c", hash, (meta ? '.' : '\0'));
        entryFile->Append(name);
        NS_ADDREF(*result = entryFile);
        return NS_OK;
    }
    return NS_ERROR_NOT_AVAILABLE;
}

nsresult nsDiskCacheDevice::getTransportForFile(nsIFile* file, nsCacheAccessMode mode, nsITransport ** result)
{
    nsresult rv;
    static NS_DEFINE_CID(kFileTransportServiceCID, NS_FILETRANSPORTSERVICE_CID);
    NS_WITH_SERVICE(nsIFileTransportService, service, kFileTransportServiceCID, &rv);
    if (NS_FAILED(rv)) return rv;
    PRInt32 ioFlags;
    switch (mode) {
    case nsICache::ACCESS_READ:
        ioFlags = PR_RDONLY;
        break;
    case nsICache::ACCESS_WRITE:
        ioFlags = PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE;
        break;
    case nsICache::ACCESS_READ_WRITE:
        ioFlags = PR_RDWR | PR_CREATE_FILE;
        break;
    }
    return service->CreateTransport(file, ioFlags, PR_IRUSR | PR_IWUSR, result);
}

/**
 * Search the cache directory for already cached entries, and add them to mCachedEntries?
 */
nsresult nsDiskCacheDevice::scanEntries()
{
    nsCOMPtr<nsISimpleEnumerator> entries;
    nsresult rv = mCacheDirectory->GetDirectoryEntries(getter_AddRefs(entries));
    if (NS_FAILED(rv)) return rv;
    
    for (PRBool more; NS_SUCCEEDED(entries->HasMoreElements(&more)) && more;) {
        nsCOMPtr<nsISupports> next;
        rv = entries->GetNext(getter_AddRefs(next));
        if (NS_FAILED(rv)) break;
        nsCOMPtr<nsIFile> file(do_QueryInterface(next, &rv));
        if (NS_FAILED(rv)) break;
        nsXPIDLCString name;
        rv = file->GetLeafName(getter_Copies(name));
        if (nsCRT::strlen(name) > 8) {
            // this must be a metadata file, read the key in, and create an inactive entry for it.
            nsCOMPtr<nsITransport> transport;
            rv = getTransportForFile(file, nsICache::ACCESS_READ, getter_AddRefs(transport));
            if (NS_FAILED(rv)) continue;
            nsCOMPtr<nsIInputStream> input;
            rv = transport->OpenInputStream(0, -1, 0, getter_AddRefs(input));
            if (NS_FAILED(rv)) continue;
            PRUint32 count;
            rv = input->Available(&count);
            if (NS_FAILED(rv)) continue;
            char* buffer = new char[count + 1];
            nsXPIDLCString owner;
            *getter_Copies(owner) = buffer;
            rv = input->Read(buffer, count, &count);
            if (NS_FAILED(rv)) continue;
            buffer[count] = '\0';
            nsCString* key = new nsCString(buffer);
            nsCacheEntry* cacheEntry = new nsCacheEntry(key, PR_TRUE, nsICache::STORE_ON_DISK);
            rv = BindEntry(cacheEntry);
        }
    }

    return NS_OK;
}

struct MetaDataHeader {
    PRUint32        mHeaderSize;
    PRInt32         mFetchCount;
    PRTime          mLastFetched;
    PRTime          mLastValidated;
    PRTime          mExpirationTime;
    PRUint32        mDataSize;
    PRUint32        mKeySize;
    PRUint32        mMetaDataSize;
    // followed by null-terminated key and metadata string values.

    MetaDataHeader()
        :   mHeaderSize(sizeof(MetaDataHeader)),
            mFetchCount(0),
            mLastFetched(LL_ZERO),
            mLastValidated(LL_ZERO),
            mExpirationTime(LL_ZERO),
            mDataSize(0),
            mKeySize(0),
            mMetaDataSize(0)
    {
    }

    MetaDataHeader(nsCacheEntry* entry)
        :   mHeaderSize(sizeof(MetaDataHeader)),
            mFetchCount(entry->FetchCount()),
            mLastFetched(entry->LastFetched()),
            mLastValidated(entry->LastValidated()),
            mExpirationTime(entry->ExpirationTime()),
            mDataSize(entry->DataSize()),
            mKeySize(entry->Key()->Length() + 1),
            mMetaDataSize(0)
    {
    }
};

struct MetaDataFile : MetaDataHeader {
    char*           mKey;
    char*           mMetaData;

    MetaDataFile()
        :   mKey(nsnull), mMetaData(nsnull)
    {
    }
    
    MetaDataFile(nsCacheEntry* entry)
        :   MetaDataHeader(entry),
            mKey(nsnull), mMetaData(nsnull)
    {
    }
    
    ~MetaDataFile()
    {
        delete[] mKey;
        delete[] mMetaData;
    }
    
    nsresult Init(nsCacheEntry* entry)
    {
        PRUint32 size = 1 + entry->Key()->Length();
        mKey = new char[size];
        if (!mKey) return NS_ERROR_OUT_OF_MEMORY;
        nsCRT::memcpy(mKey, entry->Key()->get(), size);
        return entry->FlattenMetaData(&mMetaData, &mMetaDataSize);
    }

    nsresult Write(nsIOutputStream* output);
    nsresult Read(nsIInputStream* input);
};

nsresult MetaDataFile::Write(nsIOutputStream* output)
{
    nsresult rv;
    PRUint32 count;
    
    rv = output->Write((char*)this, mHeaderSize, &count);
    if (NS_FAILED(rv)) return rv;
    
    // write the key to the file.
    rv = output->Write(mKey, mKeySize, &count);
    if (NS_FAILED(rv)) return rv;
    
    // write the flattened metadata to the file.
    if (mMetaDataSize) {
        rv = output->Write(mMetaData, mMetaDataSize, &count);
        if (NS_FAILED(rv)) return rv;
    }
    
    return NS_OK;
}

nsresult MetaDataFile::Read(nsIInputStream* input)
{
    nsresult rv;
    PRUint32 count;
    
    // read the header size used by this file.
    rv = input->Read((char*)&mHeaderSize, sizeof(mHeaderSize), &count);
    if (NS_FAILED(rv)) return rv;
    rv = input->Read((char*)&mFetchCount, mHeaderSize - sizeof(mHeaderSize), &count);
    if (NS_FAILED(rv)) return rv;

    // read in the key.
    mKey = new char[mKeySize];
    if (!mKey) return NS_ERROR_OUT_OF_MEMORY;
    rv = input->Read(mKey, mKeySize, &count);
    if (NS_FAILED(rv)) return rv;

    // read in the metadata.
    if (mMetaDataSize) {
        mMetaData = new char[mMetaDataSize];
        if (!mMetaData) return NS_ERROR_OUT_OF_MEMORY;
        rv = input->Read(mMetaData, mMetaDataSize, &count);
        if (NS_FAILED(rv)) return rv;
    }
    
    return NS_OK;
}

// XXX All these transports and opening/closing of files. We need a way to cache open files,
// XXX and to seek. Perhaps I should just be using ANSI FILE objects for all of the metadata
// XXX operations.

nsresult nsDiskCacheDevice::updateDiskCacheEntry(nsCacheEntry* entry)
{
    DiskCacheEntry* diskEntry = getDiskCacheEntry(entry);
    if (diskEntry && diskEntry->isDirty()) {
        nsresult rv;
        nsCOMPtr<nsITransport>& transport = diskEntry->getMetaTransport(nsICache::ACCESS_WRITE);
        if (!transport) {
            nsCOMPtr<nsIFile> file;
            rv = getFileForKey(entry->Key()->get(), PR_TRUE, getter_AddRefs(file));
            if (NS_FAILED(rv)) return rv;
            
            rv = getTransportForFile(file, nsICache::ACCESS_WRITE, getter_AddRefs(transport));
            if (NS_FAILED(rv)) return rv;
        }

        nsCOMPtr<nsIOutputStream> output;
        rv = transport->OpenOutputStream(0, -1, 0, getter_AddRefs(output));
        if (NS_FAILED(rv)) return rv;
        
        // write the metadata to the file.
        MetaDataFile metaDataFile(entry);
        rv = metaDataFile.Init(entry);
        if (NS_FAILED(rv)) return rv;
        rv = metaDataFile.Write(output);
        
        rv = output->Close();
        
        // mark the disk entry as being consistent with meta data file.
        diskEntry->setDirty(PR_FALSE);
    }
    return NS_OK;
}

nsresult nsDiskCacheDevice::readDiskCacheEntry(nsCString* key, nsCacheEntry ** result)
{
    // up front, check to see if the file we are looking for exists.
    nsCOMPtr<nsIFile> file;
    nsresult rv = getFileForKey(key->get(), PR_TRUE, getter_AddRefs(file));
    if (NS_FAILED(rv)) return rv;
    PRBool exists;
    rv = file->Exists(&exists);
    if (NS_FAILED(rv) || !exists) return NS_ERROR_NOT_AVAILABLE;

    nsCString* newKey = new nsCString(key->get());
    if (!newKey) return NS_ERROR_OUT_OF_MEMORY;
    
    nsCacheEntry* entry = new nsCacheEntry(newKey, PR_TRUE, nsICache::STORE_ON_DISK);
    if (!entry) { delete newKey; return NS_ERROR_OUT_OF_MEMORY; }
        
    do {
        DiskCacheEntry* diskEntry = ensureDiskCacheEntry(entry);
        if (!diskEntry) break;

        nsCOMPtr<nsITransport>& transport = diskEntry->getMetaTransport(nsICache::ACCESS_READ);
        if (!transport) {
            rv = getTransportForFile(file, nsICache::ACCESS_READ, getter_AddRefs(transport));
            if (NS_FAILED(rv)) break;
        }

        nsCOMPtr<nsIInputStream> input;
        rv = transport->OpenInputStream(0, -1, 0, getter_AddRefs(input));
        if (NS_FAILED(rv)) break;
        
        // read the metadata file.
        MetaDataFile metaDataFile;
        rv = metaDataFile.Read(input);
        input->Close();
        if (NS_FAILED(rv)) break;
        
        // initialize the entry.
        entry->SetFetchCount(metaDataFile.mFetchCount);
        entry->SetLastFetched(metaDataFile.mLastFetched);
        entry->SetLastValidated(metaDataFile.mLastValidated);
        entry->SetLastValidated(metaDataFile.mLastValidated);
        entry->SetExpirationTime(metaDataFile.mExpirationTime);
        entry->SetDataSize(metaDataFile.mDataSize);
        
        // restore the metadata.
        if (metaDataFile.mMetaDataSize) {
            rv = entry->UnflattenMetaData(metaDataFile.mMetaData, metaDataFile.mMetaDataSize);
            if (NS_FAILED(rv)) break;
        }
        
        // celebrate!        
        *result = entry;
        return NS_OK;
    } while (0);

    // oh, auto_ptr<> would be nice right about now.    
    delete entry;
    return rv;
}

//** need methods for enumerating entries
