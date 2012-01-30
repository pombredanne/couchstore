#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include "couch_db.h"
#include "couch_btree.h"
#include "ei.h"
#include "snappy-c.h"
#include "util.h"
#include "reduces.h"

sized_buf nil_atom = {
    "\x64\x00\x03nil",
    6
};

int find_header(Db *db)
{
    int block = db->file_pos / SIZE_BLOCK;
    int errcode = 0;
    int readsize;
    char* header_buf = NULL;
    uint8_t buf[2];

    while(block >= 0)
    {
        readsize = pread(db->fd, buf, 2, block * SIZE_BLOCK);
        if(readsize == 2 && buf[0] == 1)
        {
            //Found a header block.
            int header_len = pread_header(db->fd, block * SIZE_BLOCK, &header_buf);
            int arity = 0;
            int purged_docs_index = 0;
            if(header_len > 0)
            {
                int index = 0;
                if(ei_decode_version(header_buf, &index, &arity) < 0)
                {
                    errcode = ERROR_PARSE_TERM;
                    break;
                }
                if(ei_decode_tuple_header(header_buf, &index, &arity) < 0)
                {
                    errcode = ERROR_PARSE_TERM;
                    break;
                }
                ei_skip_term(header_buf, &index); //db_header
                ei_decode_ulong(header_buf, &index, &db->header.disk_version);
                ei_decode_ulonglong(header_buf, &index, &db->header.update_seq);
                ei_skip_term(header_buf, &index); //unused
                db->header.by_id_root = read_root(header_buf, &index);
                db->header.by_seq_root = read_root(header_buf, &index);
                db->header.local_docs_root = read_root(header_buf, &index);
                ei_decode_ulonglong(header_buf, &index, &db->header.purge_seq);

                purged_docs_index = index;
                ei_skip_term(header_buf, &index); //purged_docs
                db->header.purged_docs = malloc(sizeof(sized_buf) + (index - purged_docs_index));
                db->header.purged_docs->buf = ((char*)db->header.purged_docs) + sizeof(sized_buf);
                memcpy(db->header.purged_docs->buf, header_buf + purged_docs_index, index - purged_docs_index);
                db->header.purged_docs->size = index - purged_docs_index;

                ei_skip_term(header_buf, &index); //security ptr
                ei_skip_term(header_buf, &index); //revs_limit
                break;
            }
        }
        block--;
    }

    if(header_buf != NULL)
        free(header_buf);

    if(block == -1)
    {
        //Didn't find a header block
        //TODO what do we do here?
        return ERROR_NO_HEADER;
    }
    return errcode;
}

int write_header(Db* db)
{
    ei_x_buff x_header;
    sized_buf writebuf;
    int errcode = 0;

    ei_x_new_with_version(&x_header);
    ei_x_encode_tuple_header(&x_header, 10);
    ei_x_encode_atom(&x_header, "db_header");
    ei_x_encode_ulonglong(&x_header, db->header.disk_version);
    ei_x_encode_ulonglong(&x_header, db->header.update_seq);
    ei_x_encode_ulonglong(&x_header, 0); //unused field
    ei_x_encode_nodepointer(&x_header, db->header.by_id_root);
    ei_x_encode_nodepointer(&x_header, db->header.by_seq_root);
    ei_x_encode_nodepointer(&x_header, db->header.local_docs_root);
    ei_x_encode_ulonglong(&x_header, db->header.purge_seq);
    ei_x_append_buf(&x_header, db->header.purged_docs->buf, db->header.purged_docs->size);
    ei_x_encode_atom(&x_header, "nil"); //security_ptr;
    ei_x_encode_ulonglong(&x_header, 1); //revs_limit
    writebuf.buf = x_header.buff;
    writebuf.size = x_header.index;
    errcode = db_write_header(db, &writebuf);
    ei_x_free(&x_header);
    return errcode;
}

int commit_all(Db* db, uint64_t options) {
    write_header(db);
    fsync(db->fd);
    return 0;
}

int open_db(char* filename, uint64_t options, Db** pDb)
{
    int errcode = 0;
    Db* db = malloc(sizeof(Db));
    *pDb = db;
    db->fd = open(filename, O_CREAT | O_RDWR, 0744);
    error_unless(db->fd, ERROR_OPEN_FILE);
    //TODO Not totally up on how to handle large files.
    //     Should we be using pread64 and the off64_t accepting functions?
    //     They don't seem to be available everywhere.
    db->file_pos = lseek(db->fd, 0, SEEK_END);
    //TODO are there some cases where we should blow up?
    //     such as not finding a header in a file that we didn't
    //     just create? (Possibly not a couch file?)
    if(db->file_pos == 0)
    {
        //Our file is empty, create and write a new empty db header.
        db->header.disk_version = LATEST_DISK_VERSION;
        db->header.update_seq = 0;
        db->header.by_id_root = NULL;
        db->header.by_seq_root = NULL;
        db->header.local_docs_root = NULL;
        db->header.purge_seq = 0;
        db->header.purged_docs = &nil_atom;
        write_header(db);
        return 0;
    }
    else
        try(find_header(db));
cleanup:
    return errcode;
}

int close_db(Db* db)
{
    int errcode = 0;
    if(db->fd)
        close(db->fd);
    db->fd = 0;

    if(db->header.by_id_root)
        free(db->header.by_id_root);
    db->header.by_id_root = NULL;

    if(db->header.by_seq_root)
        free(db->header.by_seq_root);
    db->header.by_seq_root = NULL;

    if(db->header.local_docs_root)
        free(db->header.local_docs_root);
    db->header.local_docs_root = NULL;

    if(db->header.purged_docs && db->header.purged_docs != &nil_atom)
        free(db->header.purged_docs);
    db->header.purged_docs = NULL;
    free(db);
    return errcode;
}

int ebin_cmp(void* k1, void* k2) {
    sized_buf *e1 = (sized_buf*)k1;
    sized_buf *e2 = (sized_buf*)k2;
    int size;
    if(e2->size < e1->size)
    {
        size = e2->size;
    }
    else
    {
        size = e1->size;
    }

    int cmp = memcmp(e1->buf, e2->buf, size);
    if(cmp == 0)
    {
        if(size < e2->size)
        {
            return -1;
        }
        else if (size < e1->size)
        {
            return 1;
        }
    }
    return cmp;
}

void* ebin_from_ext(compare_info* c, char* buf, int pos) {
    int binsize;
    int type;
    sized_buf* ebcmp = c->arg;
    ei_get_type(buf, &pos, &type, &binsize);
    ebcmp->buf = buf + pos + 5;
    ebcmp->size = binsize;
    return ebcmp;
}

void* term_from_ext(compare_info* c, char* buf, int pos) {
    int endpos = pos;
    sized_buf* ebcmp = c->arg;
    ei_skip_term(buf, &endpos);
    ebcmp->buf = buf + pos;
    ebcmp->size = endpos - pos;
    return ebcmp;
}

int long_term_cmp(void *k1, void *k2) {
    sized_buf *e1 = (sized_buf*)k1;
    sized_buf *e2 = (sized_buf*)k2;
    int pos = 0;
    uint64_t e1val, e2val;
    ei_decode_ulonglong(e1->buf, &pos, &e1val);
    pos = 0;
    ei_decode_ulonglong(e2->buf, &pos, &e2val);
    if(e1val == e2val)
    {
        return 0;
    }
    return (e1val < e2val ? -1 : 1);
}

int docinfo_from_buf(DocInfo** pInfo, sized_buf *v, int idBytes)
{
    int errcode = 0,term_index = 0, fterm_pos = 0, fterm_size = 0;
    int metabin_pos = 0, metabin_size = 0;
    unsigned long deleted;
    uint64_t seq = 0, rev = 0, bp = 0;
    uint64_t size;
    *pInfo = NULL;

    if(v == NULL)
    {
        return DOC_NOT_FOUND;
    }

    //Id/Seq
    error_unless(tuple_check(v->buf, &term_index, 5), ERROR_PARSE_TERM);
    fterm_pos = term_index; //Save position of first term
    ei_skip_term(v->buf, &term_index);
    fterm_size = term_index - fterm_pos; //and size.

    //Rev = {RevNum, MetaBin}
    error_unless(tuple_check(v->buf, &term_index, 2), ERROR_PARSE_TERM);
    error_nonzero(ei_decode_ulonglong(v->buf, &term_index, &rev), ERROR_PARSE_TERM);
    metabin_pos = term_index + 5; //Save position of meta term
                                  //We know it's an ERL_BINARY_EXT, so the contents are from
                                  //5 bytes in to the end of the term.
    ei_skip_term(v->buf, &term_index);
    metabin_size = term_index - metabin_pos; //and size.

    error_nonzero(ei_decode_ulonglong(v->buf, &term_index, &bp), ERROR_PARSE_TERM);
    error_nonzero(ei_decode_ulong(v->buf, &term_index, &deleted), ERROR_PARSE_TERM);
    error_nonzero(ei_decode_ulonglong(v->buf, &term_index, &size), ERROR_PARSE_TERM);

    //If first term is seq, we don't need to include it in the buffer
    if(idBytes != 0) fterm_size = 0;
    char* infobuf = malloc(sizeof(DocInfo) + metabin_size + fterm_size + idBytes);
    *pInfo = (DocInfo*) infobuf;

    (*pInfo)->meta.buf = infobuf + sizeof(DocInfo);
    (*pInfo)->meta.size = metabin_size;

    (*pInfo)->id.buf = infobuf + sizeof(DocInfo) + metabin_size;

    if(idBytes != 0) //First term is Seq
    {

        (*pInfo)->id.size = idBytes;
        ei_decode_ulonglong(v->buf, &fterm_pos, &seq);
        //Let the caller fill in the Id.
    }
    else //First term is Id
    {
        (*pInfo)->id.size = fterm_size - 5; //Id will be a binary.
        memcpy((*pInfo)->id.buf, v->buf + fterm_pos + 5, fterm_size);
        //Let the caller fill in the Seq
    }

    (*pInfo)->seq = seq;
    (*pInfo)->rev = rev;
    (*pInfo)->bp = bp;
    (*pInfo)->size = size;
    (*pInfo)->deleted = deleted;

cleanup:
    if(errcode < 0 && (*pInfo))
    {
        free(*pInfo);
        *pInfo = NULL;
    }
    return errcode;
}

//Fill in doc from reading file.
int bp_to_doc(Doc **pDoc, int fd, off_t bp)
{
    int errcode = 0;
    uint32_t jsonlen, hasbin;
    size_t jsonlen_uncompressed = 0;
    size_t bodylen = 0;
    char *docbody = NULL;

    bodylen = pread_bin(fd, bp, &docbody);
    error_unless(bodylen > 0, ERROR_READ);

    memcpy(&jsonlen, docbody, 4);
    jsonlen = ntohl(jsonlen);
    hasbin = jsonlen & 0x80000000;
    jsonlen = jsonlen & ~0x80000000;
    if(!hasbin)
        jsonlen = bodylen - 4; //Should be true anyway..

    //couch uncompress
    if(docbody[4] == 1) //Need to unsnappy;
    {
        error_unless(snappy_uncompressed_length(docbody + 5, jsonlen - 1, &jsonlen_uncompressed) == SNAPPY_OK, ERROR_READ)
    }
    //Fill out doc structure.
    char *docbuf = malloc(sizeof(Doc) + (bodylen - 4) + jsonlen_uncompressed); //meta and binary and json
    error_unless(docbuf, ERROR_ALLOC_FAIL);

    *pDoc = (Doc*) docbuf;
    docbuf += sizeof(Doc);

    memcpy(docbuf, docbody + 4, bodylen - 4);
    if(hasbin)
    {
        (*pDoc)->binary.buf = docbuf + jsonlen;
    }
    else
    {
        (*pDoc)->binary.buf = NULL;
    }
    (*pDoc)->binary.size = (bodylen - 4) - jsonlen;

    if(docbody[4] == 1)
    {
        (*pDoc)->json.buf = docbuf + (bodylen -4);
        error_unless(
                snappy_uncompress(docbody + 5, jsonlen - 1, (*pDoc)->json.buf, &jsonlen_uncompressed) == SNAPPY_OK, ERROR_READ);
        (*pDoc)->json.buf += 6;
        (*pDoc)->json.size = jsonlen_uncompressed - 6;
    }
    else
    {
        (*pDoc)->binary.size = (bodylen - 4) - jsonlen;
        (*pDoc)->json.buf = docbuf + 6;
        (*pDoc)->json.size = jsonlen - 6;
    }

    free(docbody);
cleanup:
    if(errcode < 0 && (*pDoc))
    {
        free(*pDoc);
        (*pDoc) = NULL;
    }
    return errcode;
}

int docinfo_fetch(couchfile_lookup_request *rq, void *k, sized_buf *v)
{
    int errcode = 0;
    sized_buf *id = k;
    DocInfo** pInfo = rq->callback_ctx;
    try(docinfo_from_buf(pInfo, v, id->size));
    memcpy((*pInfo)->id.buf, id->buf, id->size);
cleanup:
    return errcode;
}

int docinfo_by_id(Db* db, uint8_t* id,  size_t idlen, DocInfo** pInfo)
{
    sized_buf key;
    void *keylist = &key;
    couchfile_lookup_request rq;
    sized_buf cmptmp;
    int errcode = 0;

    if(db->header.by_id_root == NULL)
        return DOC_NOT_FOUND;

    key.buf = (char*) id;
    key.size = idlen;

    rq.cmp.compare = ebin_cmp;
    rq.cmp.from_ext = ebin_from_ext;
    rq.cmp.arg = &cmptmp;
    rq.fd = db->fd;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = pInfo;
    rq.fetch_callback = docinfo_fetch;
    rq.fold = 0;

    errcode = btree_lookup(&rq, db->header.by_id_root->pointer);
    if(errcode == 0)
    {
        if(*pInfo == NULL)
            errcode = DOC_NOT_FOUND;
    }
    return errcode;
}

int open_doc_with_docinfo(Db* db, DocInfo* docinfo, Doc** pDoc, uint64_t options)
{
    int errcode = 0;
    *pDoc = NULL;
    if(docinfo->deleted == 1)
        return DOC_NOT_FOUND;
    try(bp_to_doc(pDoc, db->fd, docinfo->bp));
    (*pDoc)->id.buf = docinfo->id.buf;
    (*pDoc)->id.size = docinfo->id.size;
cleanup:
    return errcode;
}

int open_doc(Db* db, uint8_t* id,  size_t idlen, Doc** pDoc, uint64_t options)
{
    int errcode = 0;
    DocInfo *info;

    *pDoc = NULL;
    try(docinfo_by_id(db, id, idlen, &info));
    try(open_doc_with_docinfo(db, info, pDoc, options));
    (*pDoc)->id.buf = (char*) id;
    (*pDoc)->id.size = idlen;

    free_docinfo(info);
cleanup:
    return errcode;
}

int byseq_do_callback(couchfile_lookup_request *rq, void *k, sized_buf *v)
{
    int(*real_callback)(Db* db, DocInfo* docinfo, void *ctx) = ((void**)rq->callback_ctx)[0];
    if(v == NULL) return 0;
    sized_buf *seqterm = k;
    int seqindex = 0;
    DocInfo* docinfo;
    docinfo_from_buf(&docinfo, v, 0);
    ei_decode_ulonglong(seqterm->buf, &seqindex, &docinfo->seq);
    if(real_callback(((void**)rq->callback_ctx)[1], docinfo, ((void**)rq->callback_ctx)[2]) != NO_FREE_DOCINFO)
        free_docinfo(docinfo);
    return 0;
}

int changes_since(Db* db, uint64_t since, uint64_t options,
        int(*f)(Db* db, DocInfo* docinfo, void *ctx), void *ctx)
{
    char since_termbuf[10];
    sized_buf since_term;
    void *keylist = &since_term;
    void *cbctx[3];
    couchfile_lookup_request rq;
    sized_buf cmptmp;
    int errcode = 0;

    if(db->header.by_seq_root == NULL)
        return 0;

    since_term.buf = since_termbuf;
    since_term.size = 0;
    ei_encode_ulonglong(since_termbuf, (int*) &since_term.size, since);

    cbctx[0] = f;
    cbctx[1] = db;
    cbctx[2] = ctx;

    rq.cmp.compare = long_term_cmp;
    rq.cmp.from_ext = term_from_ext;
    rq.cmp.arg = &cmptmp;
    rq.fd = db->fd;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = cbctx;
    rq.fetch_callback = byseq_do_callback;
    rq.fold = 1;

    errcode = btree_lookup(&rq, db->header.by_seq_root->pointer);
    return errcode;
}

void free_doc(Doc* doc)
{
    free(doc);
}

void free_docinfo(DocInfo* docinfo)
{
    free(docinfo);
}

void copy_term(char* dst, int *index, sized_buf* term)
{
    memcpy(dst + *index, term->buf, term->size);
    *index += term->size;
}

int assemble_index_value(DocInfo* docinfo, char* dst, sized_buf* first_term)
{
    int pos = 0;
    ei_encode_tuple_header(dst, &pos, 5); //2 bytes.

    //Id or Seq (possibly encoded as a binary)
    copy_term(dst, &pos, first_term); //first_term.size
    //Rev
    ei_encode_tuple_header(dst, &pos, 2); //3 bytes.
    ei_encode_ulonglong(dst, &pos, docinfo->rev); //Max 10 bytes
    ei_encode_binary(dst, &pos, docinfo->meta.buf, docinfo->meta.size); //meta.size + 5
    //Bp
    ei_encode_ulonglong(dst, &pos, docinfo->bp); //Max 10 bytes
    //Deleted
    ei_encode_ulonglong(dst, &pos, docinfo->deleted); //2 bytes
    //Size
    ei_encode_ulonglong(dst, &pos, docinfo->rev); //Max 10 bytes

    //Max 42 + first_term.size + meta.size bytes.
    return pos;
}

int write_doc(Db* db, Doc* doc, uint64_t* bp)
{
    int errcode = 0;
    int binflag = 0;
    size_t jsonlen = snappy_max_compressed_length(doc->json.size + 6);
    size_t max_size = 10 + doc->binary.size + jsonlen;

    sized_buf docbody;
    docbody.size = 4; //Set up space for size prefix;
    docbody.buf = malloc(max_size);
    error_unless(docbody.buf, ERROR_ALLOC_FAIL);

    if(doc->json.size > SNAPPY_THRESHOLD)
    {
        int jbinpos = 0;
        char* jbinbuf = malloc(doc->json.size + 6);
        error_unless(jbinbuf, ERROR_ALLOC_FAIL);
        ei_encode_version(jbinbuf, (int*) &jbinpos);
        ei_encode_binary(jbinbuf, (int*) &jbinpos, doc->json.buf, doc->json.size);

        docbody.buf[4] = 1;
        error_unless(snappy_compress(jbinbuf, jbinpos, docbody.buf + 5, &jsonlen) == SNAPPY_OK, ERROR_WRITE);

        jsonlen += 1;
        docbody.size += jsonlen;
        free(jbinbuf);
    }
    else
    {
        ei_encode_version(docbody.buf, (int*) &docbody.size);
        ei_encode_binary(docbody.buf, (int*) &docbody.size, doc->json.buf, doc->json.size);
        jsonlen = doc->json.size + 6;
    }

    memcpy(docbody.buf + docbody.size, doc->binary.buf, doc->binary.size);
    docbody.size += doc->binary.size;

    binflag = (doc->binary.size > 0) ? 0x80000000 : 0;
    *((uint32_t*) docbody.buf) = htonl((jsonlen) | binflag);

    try(db_write_buf(db, &docbody, (off_t*) bp));

cleanup:
    if(docbody.buf)
        free(docbody.buf);

    return errcode;
}

int id_action_compare(const void* actv1, const void* actv2)
{
    const couchfile_modify_action *act1, *act2;
    act1 = actv1;
    act2 = actv2;

    int cmp = ebin_cmp(act1->cmp_key, act2->cmp_key);
    if(cmp == 0)
    {
        if(act1->type < act2->type)
            return -1;
        if(act1->type > act2->type)
            return 1;
    }
    return cmp;
}


int seq_action_compare(const void* actv1, const void* actv2)
{
    const couchfile_modify_action *act1, *act2;
    act1 = actv1;
    act2 = actv2;

    uint64_t seq1, seq2;
    int pos = 0;

    ei_decode_ulonglong(act1->key->buf, &pos, &seq1);
    pos = 0;
    ei_decode_ulonglong(act2->key->buf, &pos, &seq2);

    if(seq1 < seq2)
        return -1;
    if(seq1 == seq2)
    {
        if(act1->type < act2->type)
            return -1;
        if(act1->type > act2->type)
            return 1;
        return 0;
    }
    if(seq1 > seq2)
        return 1;
    return 0;
}

typedef struct _idxupdatectx {
    couchfile_modify_action* seqacts;
    int actpos;

    sized_buf** seqs;
    sized_buf** seqvals;
    int valpos;

    fatbuf *deltermbuf;
} index_update_ctx;

void idfetch_update_cb(couchfile_modify_request* rq, sized_buf* k, sized_buf* v, void *arg)
{
    //v contains a seq we need to remove ( {Seq,_,_,_,_} )
    int termpos = 0;
    uint64_t oldseq;
    sized_buf* delbuf = NULL;
    index_update_ctx* ctx = arg;

    if(v == NULL) { //Doc not found
        return;
    }

    ei_decode_tuple_header(v->buf, &termpos, NULL);
    ei_decode_ulonglong(v->buf, &termpos, &oldseq);

    delbuf = fatbuf_get(ctx->deltermbuf, sizeof(sized_buf));
    delbuf->buf = fatbuf_get(ctx->deltermbuf, 10);
    delbuf->size = 0;
    ei_encode_ulonglong(delbuf->buf, (int*) &delbuf->size, oldseq);

    ctx->seqacts[ctx->actpos].type = ACTION_REMOVE;
    ctx->seqacts[ctx->actpos].value.term = NULL;
    ctx->seqacts[ctx->actpos].key = delbuf;
    ctx->seqacts[ctx->actpos].cmp_key = delbuf;

    ctx->actpos++;

    return;
}

int update_indexes(Db* db, sized_buf* seqs, sized_buf* seqvals, sized_buf* ids, sized_buf* idvals, int numdocs)
{
    int errcode = 0;
    fatbuf* actbuf = fatbuf_alloc(numdocs * ( 4 * sizeof(couchfile_modify_action) + // Two action list up to numdocs * 2 in size
                                              2 * sizeof(sized_buf) + // Compare keys for ids, and compare keys for removed seqs found from id index.
                                              10)); //Max size of a longlong erlang term (for deleted seqs)
    sized_buf* idcmps;
    couchfile_modify_action *idacts, *seqacts;
    node_pointer *new_id_root, *new_seq_root;

    idacts = (couchfile_modify_action*) fatbuf_get(actbuf, numdocs * sizeof(couchfile_modify_action) * 2);
    seqacts = (couchfile_modify_action*) fatbuf_get(actbuf, numdocs * sizeof(couchfile_modify_action) * 2);
    idcmps = fatbuf_get(actbuf, numdocs * sizeof(sized_buf));

    couchfile_modify_request seqrq, idrq;
    sized_buf tmpsb;
    index_update_ctx fetcharg = {
        seqacts, 0, &seqs, &seqvals, 0,
        actbuf};


    int i;
    for(i = 0; i < numdocs; i++)
    {
        idcmps[i].buf = ids[i].buf + 5;
        idcmps[i].size = ids[i].size - 5;

        idacts[i * 2].type = ACTION_FETCH;
        idacts[i * 2].value.arg = &fetcharg;
        idacts[i * 2 + 1].type = ACTION_INSERT;
        idacts[i * 2 + 1].value.term = &idvals[i];

        idacts[i * 2].key = &ids[i];
        idacts[i * 2].cmp_key = &idcmps[i];

        idacts[i * 2 + 1].key = &ids[i];
        idacts[i * 2 + 1].cmp_key = &idcmps[i];
    }


    qsort(idacts, numdocs * 2, sizeof(couchfile_modify_action), id_action_compare);

    idrq.cmp.compare = ebin_cmp;
    idrq.cmp.from_ext = ebin_from_ext;
    idrq.cmp.arg = &tmpsb;
    idrq.fd = db->fd;
    idrq.actions = idacts;
    idrq.num_actions = numdocs * 2;
    idrq.reduce = by_id_reduce;
    idrq.rereduce = by_id_rereduce;
    idrq.fetch_callback = idfetch_update_cb;
    idrq.db = db;

    new_id_root = modify_btree(&idrq, db->header.by_id_root, &errcode);
    try(errcode);

    while(fetcharg.valpos < numdocs)
    {
        seqacts[fetcharg.actpos].type = ACTION_INSERT;
        seqacts[fetcharg.actpos].value.term = &seqvals[fetcharg.valpos];
        seqacts[fetcharg.actpos].key = &seqs[fetcharg.valpos];
        seqacts[fetcharg.actpos].cmp_key = &seqs[fetcharg.valpos];
        fetcharg.valpos++;
        fetcharg.actpos++;
    }

    //printf("Total seq actions: %d\n", fetcharg.actpos);
    qsort(seqacts, fetcharg.actpos, sizeof(couchfile_modify_action), seq_action_compare);

    seqrq.cmp.compare = long_term_cmp;
    seqrq.cmp.from_ext = term_from_ext;
    seqrq.cmp.arg = &tmpsb;
    seqrq.fd = db->fd;
    seqrq.actions = seqacts;
    seqrq.num_actions = fetcharg.actpos;
    seqrq.reduce = by_seq_reduce;
    seqrq.rereduce = by_seq_rereduce;
    seqrq.db = db;

    new_seq_root = modify_btree(&seqrq, db->header.by_seq_root, &errcode);
    try(errcode);

    if(db->header.by_id_root != new_id_root)
    {
        free(db->header.by_id_root);
        db->header.by_id_root = new_id_root;
    }

    if(db->header.by_seq_root != new_seq_root)
    {
        free(db->header.by_seq_root);
        db->header.by_seq_root = new_seq_root;
    }

cleanup:
    fatbuf_free(actbuf);
    return errcode;
}

int add_doc_to_update_list(Db* db, Doc* doc, DocInfo* info, fatbuf* fb,
        sized_buf* seqterm, sized_buf* idterm, sized_buf* seqval, sized_buf* idval, uint64_t seq)
{
    int errcode = 0;
    DocInfo new = *info;
    new.seq = seq;

    seqterm->buf = fatbuf_get(fb, 10);
    seqterm->size = 0;

    error_unless(seqterm->buf, ERROR_ALLOC_FAIL);
    ei_encode_ulonglong(seqterm->buf, (int*) &seqterm->size, seq);

    if(doc && !info->deleted)
    {
        try(write_doc(db, doc, &new.bp));
    }
    else
    {
        new.deleted = 1;
        new.bp = 0;
    }

    idterm->buf = fatbuf_get(fb, new.id.size + 5);
    error_unless(idterm->buf, ERROR_ALLOC_FAIL);
    idterm->size = 0;
    ei_encode_binary(idterm->buf, (int*) &idterm->size, new.id.buf, new.id.size);

    seqval->buf = fatbuf_get(fb, (42 + new.id.size + new.meta.size));
    error_unless(seqval->buf, ERROR_ALLOC_FAIL);
    seqval->size = assemble_index_value(&new, seqval->buf, idterm);

    idval->buf = fatbuf_get(fb, (42 + 10 + new.meta.size));
    error_unless(idval->buf, ERROR_ALLOC_FAIL);
    idval->size = assemble_index_value(&new, idval->buf, seqterm);

    //Use max of 10 +, id.size + 5 +, 42 + meta.size + id.size, + 52 + meta.size
    // == id.size *2 + meta.size *2 + 109 bytes
cleanup:
    return errcode;
}

int save_docs(Db* db, Doc* docs, DocInfo* infos, long numdocs, uint64_t options)
{
    int errcode = 0, i;
    sized_buf *seqklist, *idklist, *seqvlist, *idvlist;
    size_t term_meta_size = 0;
    Doc* curdoc;
    uint64_t seq = db->header.update_seq;

    fatbuf* fb;

    for(i = 0; i < numdocs; i++)
    {
        //Get additional size for terms to be inserted into indexes
        term_meta_size += 109 + (2 * (infos[i].id.size + infos[i].meta.size));
    }

    fb = fatbuf_alloc(term_meta_size +
                numdocs * (
                sizeof(sized_buf) * 4)); //seq/id key and value lists

    error_unless(fb, ERROR_ALLOC_FAIL);

    seqklist = fatbuf_get(fb, numdocs * sizeof(sized_buf));
    idklist = fatbuf_get(fb, numdocs * sizeof(sized_buf));
    seqvlist = fatbuf_get(fb, numdocs * sizeof(sized_buf));
    idvlist = fatbuf_get(fb, numdocs * sizeof(sized_buf));

    for(i = 0; i < numdocs; i++)
    {
        seq++;
        if(docs)
            curdoc = &docs[i];
        else
            curdoc = NULL;
        try(add_doc_to_update_list(db, curdoc, &infos[i], fb,
                    &seqklist[i], &idklist[i], &seqvlist[i], &idvlist[i], seq));
    }

    try(update_indexes(db, seqklist, seqvlist, idklist, idvlist, numdocs));

cleanup:
    if(fb)
        fatbuf_free(fb);
    if(errcode == 0)
        db->header.update_seq = seq;
    return errcode;
}

int save_doc(Db* db, Doc* doc, DocInfo* info, uint64_t options)
{
    return save_docs(db, doc, info, 1, options);
}

