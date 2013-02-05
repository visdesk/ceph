#include <errno.h>
#include <string.h>

#include "common/ceph_crypto.h"
#include "common/Formatter.h"
#include "common/utf8.h"

#include "rgw_rest.h"
#include "rgw_rest_s3.h"
#include "rgw_acl.h"
#include "rgw_policy_s3.h"
#include "rgw_user.h"

#include "common/armor.h"

#include "rgw_client_io.h"

#define dout_subsys ceph_subsys_rgw

using namespace ceph::crypto;

void dump_common_s3_headers(struct req_state *s, const char *etag,
                            size_t content_len, const char *conn_status)
{
  // how many elements do we expect to include in the response
  unsigned int expected_var_len = 4;
  map<string, string> head_var;

  utime_t date = ceph_clock_now(s->cct);
  if (!date.is_zero()) {
    char buf[TIME_BUF_SIZE];
    date.sprintf(buf, TIME_BUF_SIZE);
    head_var["date"] = buf;
  }

  head_var["etag"] = etag;
  head_var["conn_stat"] = conn_status;
  head_var["server"] = s->env->get("HTTP_HOST");

  // if we have all the variables we want go ahead and dump
  if (head_var.size() == expected_var_len) {
    dump_pair(s, "Date", head_var["date"].c_str());
    dump_etag(s, head_var["etag"].c_str());
    dump_content_length(s, content_len);
    dump_pair(s, "Connection", head_var["conn_stat"].c_str());
    dump_pair(s, "Server", head_var["server"].c_str());
  }
}

void list_all_buckets_start(struct req_state *s)
{
  s->formatter->open_array_section_in_ns("ListAllMyBucketsResult",
			      "http://s3.amazonaws.com/doc/2006-03-01/");
}

void list_all_buckets_end(struct req_state *s)
{
  s->formatter->close_section();
}

void dump_bucket(struct req_state *s, RGWBucketEnt& obj)
{
  s->formatter->open_object_section("Bucket");
  s->formatter->dump_string("Name", obj.bucket.name);
  dump_time(s, "CreationDate", &obj.mtime);
  s->formatter->close_section();
}

void rgw_get_errno_s3(rgw_html_errors *e , int err_no)
{
  const struct rgw_html_errors *r;
  r = search_err(err_no, RGW_HTML_ERRORS, ARRAY_LEN(RGW_HTML_ERRORS));

  if (r) {
    e->http_ret = r->http_ret;
    e->s3_code = r->s3_code;
  } else {
    e->http_ret = 500;
    e->s3_code = "UnknownError";
  }
}

struct response_attr_param {
  const char *param;
  const char *http_attr;
};

static struct response_attr_param resp_attr_params[] = {
  {"response-content-type", "Content-Type"},
  {"response-content-language", "Content-Language"},
  {"response-expires", "Expires"},
  {"response-cache-control", "Cache-Control"},
  {"response-content-disposition", "Content-Disposition"},
  {"response-content-encoding", "Content-Encoding"},
  {NULL, NULL},
};

int RGWGetObj_ObjStore_S3::send_response_data(bufferlist& bl)
{
  const char *content_type = NULL;
  string content_type_str;
  int orig_ret = ret;
  map<string, string> response_attrs;
  map<string, string>::iterator riter;

  if (ret)
    goto done;

  if (sent_header)
    goto send_data;

  if (range_str)
    dump_range(s, start, end, s->obj_size);

  dump_content_length(s, total_len);
  dump_last_modified(s, lastmod);

  if (!ret) {
    map<string, bufferlist>::iterator iter = attrs.find(RGW_ATTR_ETAG);
    if (iter != attrs.end()) {
      bufferlist& bl = iter->second;
      if (bl.length()) {
        char *etag = bl.c_str();
        dump_etag(s, etag);
      }
    }

    for (struct response_attr_param *p = resp_attr_params; p->param; p++) {
      bool exists;
      string val = s->args.get(p->param, &exists);
      if (exists) {
	if (strcmp(p->param, "response-content-type") != 0) {
	  response_attrs[p->http_attr] = val;
	} else {
	  content_type_str = val;
	  content_type = content_type_str.c_str();
	}
      }
    }

    for (iter = attrs.begin(); iter != attrs.end(); ++iter) {
      const char *name = iter->first.c_str();
      map<string, string>::iterator aiter = rgw_to_http_attrs.find(name);
      if (aiter != rgw_to_http_attrs.end()) {
	if (response_attrs.count(aiter->second) > 0) // was already overridden by a response param
	  continue;

	if ((!content_type) && aiter->first.compare(RGW_ATTR_CONTENT_TYPE) == 0) { // special handling for content_type
	  content_type = iter->second.c_str();
	  continue;
        }
	response_attrs[aiter->second] = iter->second.c_str();
      } else {
        if (strncmp(name, RGW_ATTR_META_PREFIX, sizeof(RGW_ATTR_META_PREFIX)-1) == 0) {
          name += sizeof(RGW_ATTR_PREFIX) - 1;
          s->cio->print("%s: %s\r\n", name, iter->second.c_str());
        }
      }
    }
  }

  if (partial_content && !ret)
    ret = STATUS_PARTIAL_CONTENT;
done:
  set_req_state_err(s, ret);

  dump_errno(s);

  for (riter = response_attrs.begin(); riter != response_attrs.end(); ++riter) {
    s->cio->print("%s: %s\n", riter->first.c_str(), riter->second.c_str());
  }

  if (!content_type)
    content_type = "binary/octet-stream";
  end_header(s, content_type);
  sent_header = true;

send_data:
  if (get_data && !orig_ret) {
    int r = s->cio->write(bl.c_str(), len);
    if (r < 0)
      return r;
  }

  return 0;
}

void RGWListBuckets_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  dump_start(s);

  list_all_buckets_start(s);
  dump_owner(s, s->user.user_id, s->user.display_name);

  map<string, RGWBucketEnt>& m = buckets.get_buckets();
  map<string, RGWBucketEnt>::iterator iter;

  s->formatter->open_array_section("Buckets");
  for (iter = m.begin(); iter != m.end(); ++iter) {
    RGWBucketEnt obj = iter->second;
    dump_bucket(s, obj);
  }
  s->formatter->close_section();
  list_all_buckets_end(s);
  dump_content_length(s, s->formatter->get_len());
  end_header(s, "application/xml");
  rgw_flush_formatter_and_reset(s, s->formatter);
}

int RGWListBucket_ObjStore_S3::get_params()
{
  prefix = s->args.get("prefix");
  marker = s->args.get("marker");
  max_keys = s->args.get("max-keys");
  ret = parse_max_keys();
  if (ret < 0) {
    return ret;
  }
  delimiter = s->args.get("delimiter");
  return 0;
}

void RGWListBucket_ObjStore_S3::send_response()
{
  if (ret < 0)
    set_req_state_err(s, ret);
  dump_errno(s);

  end_header(s, "application/xml");
  dump_start(s);
  if (ret < 0)
    return;

  s->formatter->open_object_section_in_ns("ListBucketResult",
					  "http://s3.amazonaws.com/doc/2006-03-01/");
  s->formatter->dump_string("Name", s->bucket_name);
  if (!prefix.empty())
    s->formatter->dump_string("Prefix", prefix);
  s->formatter->dump_string("Marker", marker);
  s->formatter->dump_int("MaxKeys", max);
  if (!delimiter.empty())
    s->formatter->dump_string("Delimiter", delimiter);

  s->formatter->dump_string("IsTruncated", (max && is_truncated ? "true" : "false"));

  if (ret >= 0) {
    vector<RGWObjEnt>::iterator iter;
    for (iter = objs.begin(); iter != objs.end(); ++iter) {
      s->formatter->open_array_section("Contents");
      s->formatter->dump_string("Key", iter->name);
      dump_time(s, "LastModified", &iter->mtime);
      s->formatter->dump_format("ETag", "\"%s\"", iter->etag.c_str());
      s->formatter->dump_int("Size", iter->size);
      s->formatter->dump_string("StorageClass", "STANDARD");
      dump_owner(s, iter->owner, iter->owner_display_name);
      s->formatter->close_section();
    }
    if (common_prefixes.size() > 0) {
      map<string, bool>::iterator pref_iter;
      for (pref_iter = common_prefixes.begin(); pref_iter != common_prefixes.end(); ++pref_iter) {
        s->formatter->open_array_section("CommonPrefixes");
        s->formatter->dump_string("Prefix", pref_iter->first);
        s->formatter->close_section();
      }
    }
  }
  s->formatter->close_section();
  rgw_flush_formatter_and_reset(s, s->formatter);
}

void RGWGetBucketLogging_ObjStore_S3::send_response()
{
  dump_errno(s);
  end_header(s, "application/xml");
  dump_start(s);

  s->formatter->open_object_section_in_ns("BucketLoggingStatus",
					  "http://doc.s3.amazonaws.com/doc/2006-03-01/");
  s->formatter->close_section();
  rgw_flush_formatter_and_reset(s, s->formatter);
}

static void dump_bucket_metadata(struct req_state *s, RGWBucketEnt& bucket)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)bucket.count);
  s->cio->print("X-RGW-Object-Count: %s\n", buf);
  snprintf(buf, sizeof(buf), "%lld", (long long)bucket.size);
  s->cio->print("X-RGW-Bytes-Used: %s\n", buf);
}

void RGWStatBucket_ObjStore_S3::send_response()
{
  if (ret >= 0) {
    dump_bucket_metadata(s, bucket);
  }

  set_req_state_err(s, ret);
  dump_errno(s);

  end_header(s);
  dump_start(s);
}

int RGWCreateBucket_ObjStore_S3::get_params()
{
  RGWAccessControlPolicy_S3 s3policy(s->cct);

  int r = s3policy.create_canned(s->owner, s->bucket_owner, s->canned_acl);
  if (r < 0)
    return r;

  policy = s3policy;

  return 0;
}

void RGWCreateBucket_ObjStore_S3::send_response()
{
  if (ret == -ERR_BUCKET_EXISTS)
    ret = 0;
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s);
}

void RGWDeleteBucket_ObjStore_S3::send_response()
{
  int r = ret;
  if (!r)
    r = STATUS_NO_CONTENT;

  set_req_state_err(s, r);
  dump_errno(s);
  end_header(s);
}

int RGWPutObj_ObjStore_S3::get_params()
{
  RGWAccessControlPolicy_S3 s3policy(s->cct);
  if (!s->length)
    return -ERR_LENGTH_REQUIRED;

  int r = s3policy.create_canned(s->owner, s->bucket_owner, s->canned_acl);
  if (!r)
     return -EINVAL;

  policy = s3policy;

  return RGWPutObj_ObjStore::get_params();
}

static int get_success_retcode(int code)
{
  switch (code) {
    case 201:
      return STATUS_CREATED;
    case 204:
      return STATUS_NO_CONTENT;
  }
  return 0;
}

void RGWPutObj_ObjStore_S3::send_response()
{
  if (ret) {
    set_req_state_err(s, ret);
  } else {
    if (s->cct->_conf->rgw_s3_success_create_obj_status) {
      ret = get_success_retcode(s->cct->_conf->rgw_s3_success_create_obj_status);
      set_req_state_err(s, ret);
    }
    dump_etag(s, etag.c_str());
    dump_content_length(s, 0);
  }
  dump_errno(s);
  end_header(s);
}

string trim_whitespace(const string& src)
{
  if (src.empty()) {
    return string();
  }

  int start = 0;
  for (; start != (int)src.size(); start++) {
    if (!isspace(src[start]))
      break;
  }

  int end = src.size() - 1;
  if (end <= start) {
    return string();
  }

  for (; end > start; end--) {
    if (!isspace(src[end]))
      break;
  }

  return src.substr(start, end - start + 1);
}

string trim_quotes(const string& val)
{
  string s = trim_whitespace(val);
  if (s.size() < 2)
    return s;

  int start = 0;
  int end = s.size() - 1;
  int quotes_count = 0;

  if (s[start] == '"') {
    start++;
    quotes_count++;
  }
  if (s[end] == '"') {
    end--;
    quotes_count++;
  }
  if (quotes_count == 2) {
    return s.substr(start, end - start + 1);
  }
  return s;
}

/*
 * parses params in the format: 'first; param1=foo; param2=bar'
 */
static void parse_params(const string& params_str, string& first, map<string, string>& params)
{
  int pos = params_str.find(';');
  if (pos < 0) {
    first = trim_whitespace(params_str);
    return;
  }

  first = trim_whitespace(params_str.substr(0, pos));

  pos++;

  while (pos < (int)params_str.size()) {
    ssize_t end = params_str.find(';', pos);
    if (end < 0)
      end = params_str.size();

    string param = params_str.substr(pos, end - pos);

    int eqpos = param.find('=');
    if (eqpos > 0) {
      string param_name = trim_whitespace(param.substr(0, eqpos));
      string val = trim_quotes(param.substr(eqpos + 1));
      params[param_name] = val;
    } else {
      params[trim_whitespace(param)] = "";
    }

    pos = end + 1;
  }
}

static int parse_part_field(const string& line, string& field_name, struct post_part_field& field)
{
  int pos = line.find(':');
  if (pos < 0)
    return -EINVAL;

  field_name = line.substr(0, pos);
  if (pos >= (int)line.size() - 1)
    return 0;

  parse_params(line.substr(pos + 1), field.val, field.params);

  return 0;
}

bool is_crlf(const char *s)
{
  return (*s == '\r' && *(s + 1) == '\n');
}

/*
 * find the index of the boundary, if exists, or optionally the next end of line
 * also returns how many bytes to skip
 */
static int index_of(bufferlist& bl, int max_len, const string& str, bool check_crlf,
                    bool *reached_boundary, int *skip)
{
  *reached_boundary = false;
  *skip = 0;

  if (str.size() < 2) // we assume boundary is at least 2 chars (makes it easier with crlf checks)
    return -EINVAL;

  if (bl.length() < str.size())
    return -1;

  const char *buf = bl.c_str();
  const char *s = str.c_str();

  if (max_len > (int)bl.length())
    max_len = bl.length();

  int i;
  for (i = 0; i < max_len; i++, buf++) {
    if (check_crlf &&
        i >= 1 &&
        is_crlf(buf - 1)) {
        return i + 1; // skip the crlf
    }
    if ((i < max_len - (int)str.size() + 1) &&
        (buf[0] == s[0] && buf[1] == s[1]) &&
        (strncmp(buf, s, str.size()) == 0)) {
      *reached_boundary = true;
      *skip = str.size();

      /* oh, great, now we need to swallow the preceding crlf
       * if exists
       */
      if ((i >= 2) &&
        is_crlf(buf - 2)) {
        i -= 2;
        *skip += 2;
      }
      return i;
    }
  }

  return -1;
}

int RGWPostObj_ObjStore_S3::read_with_boundary(bufferlist& bl, uint64_t max, bool check_crlf,
                                               bool *reached_boundary, bool *done)
{
  uint64_t cl = max + 2 + boundary.size();

  if (max > in_data.length()) {
    uint64_t need_to_read = cl - in_data.length();

    bufferptr bp(need_to_read);

    int read_len;
    s->cio->read(bp.c_str(), need_to_read, &read_len);

    in_data.append(bp, 0, read_len);
  }

  *done = false;
  int skip;
  int index = index_of(in_data, cl, boundary, check_crlf, reached_boundary, &skip);
  if (index >= 0)
    max = index;

  if (max > in_data.length())
    max = in_data.length();

  bl.substr_of(in_data, 0, max);

  bufferlist new_read_data;

  /*
   * now we need to skip boundary for next time, also skip any crlf, or
   * check to see if it's the last final boundary (marked with "--" at the end
   */
  if (*reached_boundary) {
    int left = in_data.length() - max;
    if (left < skip + 2) {
      int need = skip + 2 - left;
      bufferptr boundary_bp(need);
      int actual;
      s->cio->read(boundary_bp.c_str(), need, &actual);
      in_data.append(boundary_bp);
    }
    max += skip; // skip boundary for next time
    if (in_data.length() >= max + 2) {
      const char *data = in_data.c_str();
      if (is_crlf(data + max)) {
	max += 2;
      } else {
        if (*(data + max) == '-' &&
            *(data + max + 1) == '-') {
          *done = true;
	  max += 2;
	}
      }
    }
  }

  new_read_data.substr_of(in_data, max, in_data.length() - max);
  in_data = new_read_data;

  return 0;
}

int RGWPostObj_ObjStore_S3::read_line(bufferlist& bl, uint64_t max,
				  bool *reached_boundary, bool *done)
{
  return read_with_boundary(bl, max, true, reached_boundary, done);
}

int RGWPostObj_ObjStore_S3::read_data(bufferlist& bl, uint64_t max,
				  bool *reached_boundary, bool *done)
{
  return read_with_boundary(bl, max, false, reached_boundary, done);
}


int RGWPostObj_ObjStore_S3::read_form_part_header(struct post_form_part *part,
                                              bool *done)
{
  bufferlist bl;
  bool reached_boundary;
  int r = read_line(bl, RGW_MAX_CHUNK_SIZE, &reached_boundary, done);
  if (r < 0)
    return r;

  if (*done) {
    return 0;
  }

  if (reached_boundary) { // skip the first boundary
    r = read_line(bl, RGW_MAX_CHUNK_SIZE, &reached_boundary, done);
    if (r < 0)
      return r;
    if (*done)
      return 0;
  }

  while (true) {
  /*
   * iterate through fields
   */
    string line = trim_whitespace(string(bl.c_str(), bl.length()));

    if (line.empty())
      break;

    struct post_part_field field;

    string field_name;
    r = parse_part_field(line, field_name, field);
    if (r < 0)
      return r;

    part->fields[field_name] = field;

    if (stringcasecmp(field_name, "Content-Disposition") == 0) {
      part->name = field.params["name"];
    }

    if (reached_boundary)
      break;

    r = read_line(bl, RGW_MAX_CHUNK_SIZE, &reached_boundary, done);
  }

  return 0;
}

bool RGWPostObj_ObjStore_S3::part_str(const string& name, string *val)
{
  map<string, struct post_form_part, ltstr_nocase>::iterator iter = parts.find(name);
  if (iter == parts.end())
    return false;

  bufferlist& data = iter->second.data;
  string str = string(data.c_str(), data.length());
  *val = trim_whitespace(str);
  return true;
}

bool RGWPostObj_ObjStore_S3::part_bl(const string& name, bufferlist *pbl)
{
  map<string, struct post_form_part, ltstr_nocase>::iterator iter = parts.find(name);
  if (iter == parts.end())
    return false;

  *pbl = iter->second.data;
  return true;
}

void RGWPostObj_ObjStore_S3::rebuild_key(string& key)
{
  static string var = "${filename}";
  int pos = key.find(var);
  if (pos < 0)
    return;

  string new_key = key.substr(0, pos);
  new_key.append(filename);
  new_key.append(key.substr(pos + var.size()));

  key = new_key;
}

int RGWPostObj_ObjStore_S3::get_params()
{
  string temp_line;
  string param;
  string old_param;
  string param_value;

  string whitespaces (" \t\f\v\n\r");

  // get the part boundary
  string req_content_type_str = s->env->get("CONTENT_TYPE");
  string req_content_type;
  map<string, string> params;

  if (s->expect_cont) {
    /* ok, here it really gets ugly. With POST, the params are embedded in the
     * request body, so we need to continue before being able to actually look
     * at them. This diverts from the usual request flow.
     */
    dump_continue(s);
    s->expect_cont = false;
  }

  parse_params(req_content_type_str, req_content_type, params);

  if (req_content_type.compare("multipart/form-data") != 0) {
    err_msg = "Request Content-Type is not multipart/form-data";
    return -EINVAL;
  }

  if (s->cct->_conf->subsys.should_gather(ceph_subsys_rgw, 20)) {
    ldout(s->cct, 20) << "request content_type_str=" << req_content_type_str << dendl;
    ldout(s->cct, 20) << "request content_type params:" << dendl;
    map<string, string>::iterator iter;
    for (iter = params.begin(); iter != params.end(); ++iter) {
      ldout(s->cct, 20) << " " << iter->first << " -> " << iter->second << dendl;
    }
  }

  ldout(s->cct, 20) << "adding bucket to policy env: " << s->bucket.name << dendl;
  env.add_var("bucket", s->bucket.name);

  map<string, string>::iterator iter = params.find("boundary");
  if (iter == params.end()) {
    err_msg = "Missing multipart boundary specification";
    return -EINVAL;
  }

  // create the boundary
  boundary = "--";
  boundary.append(iter->second);

  bool done;
  do {
    struct post_form_part part;
    int r = read_form_part_header(&part, &done);
    if (r < 0)
      return r;
    
    if (s->cct->_conf->subsys.should_gather(ceph_subsys_rgw, 20)) {
      map<string, struct post_part_field, ltstr_nocase>::iterator piter;
      for (piter = part.fields.begin(); piter != part.fields.end(); ++piter) {
        ldout(s->cct, 20) << "read part header: name=" << part.name << " content_type=" << part.content_type << dendl;
        ldout(s->cct, 20) << "name=" << piter->first << dendl;
        ldout(s->cct, 20) << "val=" << piter->second.val << dendl;
        ldout(s->cct, 20) << "params:" << dendl;
        map<string, string>& params = piter->second.params;
        for (iter = params.begin(); iter != params.end(); ++iter) {
          ldout(s->cct, 20) << " " << iter->first << " -> " << iter->second << dendl;
        }
      }
    }

    if (done) { /* unexpected here */
      err_msg = "Malformed request";
      return -EINVAL;
    }

    if (stringcasecmp(part.name, "file") == 0) { /* beginning of data transfer */
      struct post_part_field& field = part.fields["Content-Disposition"];
      map<string, string>::iterator iter = field.params.find("filename");
      if (iter != field.params.end()) {
        filename = iter->second;
      }
      parts[part.name] = part;
      data_pending = true;
      break;
    }

    bool boundary;
    r = read_data(part.data, RGW_MAX_CHUNK_SIZE, &boundary, &done);
    if (!boundary) {
      err_msg = "Couldn't find boundary";
      return -EINVAL;
    }
    parts[part.name] = part;
    string part_str(part.data.c_str(), part.data.length());
    env.add_var(part.name, part_str);
  } while (!done);

  if (!part_str("key", &s->object_str)) {
    err_msg = "Key not specified";
    return -EINVAL;
  }

  rebuild_key(s->object_str);

  env.add_var("key", s->object_str);

  part_str("Content-Type", &content_type);
  env.add_var("Content-Type", content_type);

  map<string, struct post_form_part, ltstr_nocase>::iterator piter = parts.upper_bound(RGW_AMZ_META_PREFIX);
  for (; piter != parts.end(); ++piter) {
    string n = piter->first;
    if (strncasecmp(n.c_str(), RGW_AMZ_META_PREFIX, sizeof(RGW_AMZ_META_PREFIX) - 1) != 0)
      break;

    string attr_name = RGW_ATTR_PREFIX;
    attr_name.append(n);

    /* need to null terminate it */
    bufferlist& data = piter->second.data;
    string str = string(data.c_str(), data.length());

    bufferlist attr_bl;
    attr_bl.append(str.c_str(), str.size() + 1);

    attrs[attr_name] = attr_bl;
  }

  int r = get_policy();
  if (r < 0)
    return r;

  min_len = post_policy.min_length;
  max_len = post_policy.max_length;

  return 0;
}

int RGWPostObj_ObjStore_S3::get_policy()
{
  bufferlist encoded_policy;
  string uid;

  if (part_bl("policy", &encoded_policy)) {

    // check that the signature matches the encoded policy
    string s3_access_key;
    if (!part_str("AWSAccessKeyId", &s3_access_key)) {
      ldout(s->cct, 0) << "No S3 access key found!" << dendl;
      err_msg = "Missing access key";
      return -EINVAL;
    }
    string signature_str;
    if (!part_str("signature", &signature_str)) {
      ldout(s->cct, 0) << "No signature found!" << dendl;
      err_msg = "Missing signature";
      return -EINVAL;
    }

    RGWUserInfo user_info;

    ret = rgw_get_user_info_by_access_key(store, s3_access_key, user_info);
    if (ret < 0) {
      ldout(s->cct, 0) << "User lookup failed!" << dendl;
      err_msg = "Bad access key / signature";
      return -EACCES;
    }

    map<string, RGWAccessKey> access_keys  = user_info.access_keys;

    map<string, RGWAccessKey>::const_iterator iter = access_keys.begin();
    string s3_secret_key = (iter->second).key;

    char calc_signature[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE];

    calc_hmac_sha1(s3_secret_key.c_str(), s3_secret_key.size(), encoded_policy.c_str(), encoded_policy.length(), calc_signature);
    bufferlist encoded_hmac;
    bufferlist raw_hmac;
    raw_hmac.append(calc_signature, CEPH_CRYPTO_HMACSHA1_DIGESTSIZE);
    raw_hmac.encode_base64(encoded_hmac);
    encoded_hmac.append((char)0); /* null terminate */

    if (signature_str.compare(encoded_hmac.c_str()) != 0) {
      ldout(s->cct, 0) << "Signature verification failed!" << dendl;
      ldout(s->cct, 0) << "expected: " << signature_str.c_str() << dendl;
      ldout(s->cct, 0) << "got: " << encoded_hmac.c_str() << dendl;
      err_msg = "Bad access key / signature";
      return -EACCES;
    }
    ldout(s->cct, 0) << "Successful Signature Verification!" << dendl;
    bufferlist decoded_policy;
    try {
      decoded_policy.decode_base64(encoded_policy);
    } catch (buffer::error& err) {
      ldout(s->cct, 0) << "failed to decode_base64 policy" << dendl;
      err_msg = "Could not decode policy";
      return -EINVAL;
    }

    decoded_policy.append('\0'); // NULL terminate

    ldout(s->cct, 0) << "POST policy: " << decoded_policy.c_str() << dendl;

    int r = post_policy.from_json(decoded_policy, err_msg);
    if (r < 0) {
      if (err_msg.empty()) {
	err_msg = "Failed to parse policy";
      }
      ldout(s->cct, 0) << "failed to parse policy" << dendl;
      return -EINVAL;
    }

    post_policy.set_var_checked("AWSAccessKeyId");
    post_policy.set_var_checked("policy");
    post_policy.set_var_checked("signature");

    r = post_policy.check(&env, err_msg);
    if (r < 0) {
      if (err_msg.empty()) {
	err_msg = "Policy check failed";
      }
      ldout(s->cct, 0) << "policy check failed" << dendl;
      return r;
    }

    s->user = user_info;
    s->owner.set_id(user_info.user_id);
    s->owner.set_name(user_info.display_name);
  } else {
    ldout(s->cct, 0) << "No attached policy found!" << dendl;
  }

  string canned_acl;
  part_str("acl", &canned_acl);

  RGWAccessControlPolicy_S3 s3policy(s->cct);
  ldout(s->cct, 20) << "canned_acl=" << canned_acl << dendl;
  if (!s3policy.create_canned(s->owner, s->bucket_owner, canned_acl)) {
    err_msg = "Bad canned ACLs";
    return -EINVAL;
  }

  policy = s3policy;

  return 0;
}

int RGWPostObj_ObjStore_S3::complete_get_params()
{
  bool done;
  do {
    struct post_form_part part;
    int r = read_form_part_header(&part, &done);
    if (r < 0)
      return r;
    
    bufferlist part_data;
    bool boundary;
    r = read_data(part.data, RGW_MAX_CHUNK_SIZE, &boundary, &done);
    if (!boundary) {
      return -EINVAL;
    }

    parts[part.name] = part;
  } while (!done);

  return 0;
}

int RGWPostObj_ObjStore_S3::get_data(bufferlist& bl)
{
  bool boundary;
  bool done;

  int r = read_data(bl, RGW_MAX_CHUNK_SIZE, &boundary, &done);
  if (r < 0)
    return r;

  if (boundary) {
    data_pending = false;

    if (!done) {  /* reached end of data, let's drain the rest of the params */
      r = complete_get_params();
      if (r < 0)
        return r;
    }
  }

  return bl.length();
}

static void escape_char(char c, string& dst)
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%%%.2X", (unsigned int)c);
  dst.append(buf);
}

static bool char_needs_url_encoding(char c)
{
  if (c < 0x20 || c >= 0x7f)
    return true;

  switch (c) {
    case 0x20:
    case 0x22:
    case 0x23:
    case 0x25:
    case 0x26:
    case 0x2B:
    case 0x2C:
    case 0x2F:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3E:
    case 0x3D:
    case 0x3F:
    case 0x40:
    case 0x5B:
    case 0x5D:
    case 0x5C:
    case 0x5E:
    case 0x60:
    case 0x7B:
    case 0x7D:
      return true;
  }
  return false;
}

static void url_escape(const string& src, string& dst)
{
  const char *p = src.c_str();
  for (unsigned i = 0; i < src.size(); i++, p++) {
    if (char_needs_url_encoding(*p)) {
      escape_char(*p, dst);
      continue;
    }

    dst.append(p, 1);
  }
}

void RGWPostObj_ObjStore_S3::send_response()
{
  if (ret == 0 && parts.count("success_action_redirect")) {
    string redirect;

    part_str("success_action_redirect", &redirect);

    string bucket;
    string key;
    string etag_str = "\"";

    etag_str.append(etag);
    etag_str.append("\"");

    string etag_url;

    url_escape(s->bucket_name, bucket);
    url_escape(s->object_str, key);
    url_escape(etag_str, etag_url);

    
    redirect.append("?bucket=");
    redirect.append(bucket);
    redirect.append("&key=");
    redirect.append(key);
    redirect.append("&etag=");
    redirect.append(etag_url);

    int r = check_utf8(redirect.c_str(), redirect.size());
    if (r < 0) {
      ret = r;
      goto done;
    }
    dump_redirect(s, redirect);
    ret = STATUS_REDIRECT;
  } else if (ret == 0 && parts.count("success_action_status")) {
    string status_string;
    uint32_t status_int;

    part_str("success_action_status", &status_string);

    int r = stringtoul(status_string, &status_int);
    if (r < 0) {
      ret = r;
      goto done;
    }

    switch (status_int) {
      case 200:
	break;
      case 201:
	ret = STATUS_CREATED;
	break;
      default:
	ret = STATUS_NO_CONTENT;
	break;
    }
  } else if (!ret) {
    ret = STATUS_NO_CONTENT;
  }

done:
  if (ret == STATUS_CREATED) {
    s->formatter->open_object_section("PostResponse");
    if (g_conf->rgw_dns_name.length())
      s->formatter->dump_format("Location", "%s/%s", s->script_uri.c_str(), s->object_str.c_str());
    s->formatter->dump_string("Bucket", s->bucket_name);
    s->formatter->dump_string("Key", s->object_str.c_str());
    s->formatter->close_section();
  }
  s->err.message = err_msg;
  set_req_state_err(s, ret);
  dump_errno(s);
  dump_content_length(s, s->formatter->get_len());
  end_header(s);
  if (ret != STATUS_CREATED)
    return;

  rgw_flush_formatter_and_reset(s, s->formatter);
}


void RGWDeleteObj_ObjStore_S3::send_response()
{
  int r = ret;
  if (r == -ENOENT)
    r = 0;
  if (!r)
    r = STATUS_NO_CONTENT;

  set_req_state_err(s, r);
  dump_errno(s);
  end_header(s);
}

int RGWCopyObj_ObjStore_S3::init_dest_policy()
{
  RGWAccessControlPolicy_S3 s3policy(s->cct);

  /* build a policy for the target object */
  ret = s3policy.create_canned(s->owner, s->bucket_owner, s->canned_acl);
  if (!ret)
     return -EINVAL;

  dest_policy = s3policy;

  return 0;
}

int RGWCopyObj_ObjStore_S3::get_params()
{
  if_mod = s->env->get("HTTP_X_AMZ_COPY_IF_MODIFIED_SINCE");
  if_unmod = s->env->get("HTTP_X_AMZ_COPY_IF_UNMODIFIED_SINCE");
  if_match = s->env->get("HTTP_X_AMZ_COPY_IF_MATCH");
  if_nomatch = s->env->get("HTTP_X_AMZ_COPY_IF_NONE_MATCH");

  const char *req_src = s->copy_source;
  if (!req_src)
    return -EINVAL;

  ret = parse_copy_location(req_src, src_bucket_name, src_object);
  if (!ret)
     return -EINVAL;

  dest_bucket_name = s->bucket.name;
  dest_object = s->object_str;

  const char *md_directive = s->env->get("HTTP_X_AMZ_METADATA_DIRECTIVE");
  if (md_directive) {
    if (strcasecmp(md_directive, "COPY") == 0) {
      replace_attrs = false;
    } else if (strcasecmp(md_directive, "REPLACE") == 0) {
      replace_attrs = true;
    } else {
      return -EINVAL;
    }
  }

  if ((dest_bucket_name.compare(src_bucket_name) == 0) &&
      (dest_object.compare(src_object) == 0) &&
      !replace_attrs) {
    /* can only copy object into itself if replacing attrs */
    return -ERR_INVALID_REQUEST;
  }
  return 0;
}

void RGWCopyObj_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);

  end_header(s, "binary/octet-stream");
  if (ret == 0) {
    s->formatter->open_object_section("CopyObjectResult");
    dump_time(s, "LastModified", &mtime);
    map<string, bufferlist>::iterator iter = attrs.find(RGW_ATTR_ETAG);
    if (iter != attrs.end()) {
      bufferlist& bl = iter->second;
      if (bl.length()) {
        char *etag = bl.c_str();
        s->formatter->dump_string("ETag", etag);
      }
    }
    s->formatter->close_section();
    rgw_flush_formatter_and_reset(s, s->formatter);
  }
}

void RGWGetACLs_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, "application/xml");
  dump_start(s);
  s->cio->write(acls.c_str(), acls.size());
}

int RGWPutACLs_ObjStore_S3::get_canned_policy(ACLOwner& owner, stringstream& ss)
{
  RGWAccessControlPolicy_S3 s3policy(s->cct);

  // bucket-* canned acls do not apply to buckets
  if (s->canned_acl.find("bucket") != string::npos) {
    if (s->object_str.empty())
      s->canned_acl = "";
  }

  bool r;
  r = s3policy.create_canned(owner, s->bucket_owner, s->canned_acl);

  if (!r)
    return -EINVAL;

  s3policy.to_xml(ss);

  return 0;
}

void RGWPutACLs_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, "application/xml");
  dump_start(s);
}

int RGWInitMultipart_ObjStore_S3::get_params()
{
  RGWAccessControlPolicy_S3 s3policy(s->cct);
  ret = s3policy.create_canned(s->owner, s->bucket_owner, s->canned_acl);
  if (!ret)
     return -EINVAL;

  policy = s3policy;

  return 0;
}

void RGWInitMultipart_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, "application/xml");
  if (ret == 0) { 
    dump_start(s);
    s->formatter->open_object_section_in_ns("InitiateMultipartUploadResult",
		  "http://s3.amazonaws.com/doc/2006-03-01/");
    s->formatter->dump_string("Bucket", s->bucket_name);
    s->formatter->dump_string("Key", s->object);
    s->formatter->dump_string("UploadId", upload_id);
    s->formatter->close_section();
    rgw_flush_formatter_and_reset(s, s->formatter);
  }
}

void RGWCompleteMultipart_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, "application/xml");
  if (ret == 0) { 
    dump_start(s);
    s->formatter->open_object_section_in_ns("CompleteMultipartUploadResult",
			  "http://s3.amazonaws.com/doc/2006-03-01/");
    if (g_conf->rgw_dns_name.length())
      s->formatter->dump_format("Location", "%s.%s", s->bucket_name, g_conf->rgw_dns_name.c_str());
    s->formatter->dump_string("Bucket", s->bucket_name);
    s->formatter->dump_string("Key", s->object);
    s->formatter->dump_string("ETag", etag);
    s->formatter->close_section();
    rgw_flush_formatter_and_reset(s, s->formatter);
  }
}

void RGWAbortMultipart_ObjStore_S3::send_response()
{
  int r = ret;
  if (!r)
    r = STATUS_NO_CONTENT;

  set_req_state_err(s, r);
  dump_errno(s);
  end_header(s);
}

void RGWListMultipart_ObjStore_S3::send_response()
{
  if (ret)
    set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, "application/xml");

  if (ret == 0) { 
    dump_start(s);
    s->formatter->open_object_section_in_ns("ListMultipartUploadResult",
		    "http://s3.amazonaws.com/doc/2006-03-01/");
    map<uint32_t, RGWUploadPartInfo>::iterator iter, test_iter;
    int i, cur_max = 0;

    iter = parts.upper_bound(marker);
    for (i = 0, test_iter = iter; test_iter != parts.end() && i < max_parts; ++test_iter, ++i) {
      cur_max = test_iter->first;
    }
    s->formatter->dump_string("Bucket", s->bucket_name);
    s->formatter->dump_string("Key", s->object);
    s->formatter->dump_string("UploadId", upload_id);
    s->formatter->dump_string("StorageClass", "STANDARD");
    s->formatter->dump_int("PartNumberMarker", marker);
    s->formatter->dump_int("NextPartNumberMarker", cur_max + 1);
    s->formatter->dump_int("MaxParts", max_parts);
    s->formatter->dump_string("IsTruncated", (test_iter == parts.end() ? "false" : "true"));

    ACLOwner& owner = policy.get_owner();
    dump_owner(s, owner.get_id(), owner.get_display_name());

    for (; iter != parts.end(); ++iter) {
      RGWUploadPartInfo& info = iter->second;

      time_t sec = info.modified.sec();
      struct tm tmp;
      gmtime_r(&sec, &tmp);
      char buf[TIME_BUF_SIZE];

      s->formatter->open_object_section("Part");

      if (strftime(buf, sizeof(buf), "%Y-%m-%dT%T.000Z", &tmp) > 0) {
        s->formatter->dump_string("LastModified", buf);
      }

      s->formatter->dump_unsigned("PartNumber", info.num);
      s->formatter->dump_string("ETag", info.etag);
      s->formatter->dump_unsigned("Size", info.size);
      s->formatter->close_section();
    }
    s->formatter->close_section();
    rgw_flush_formatter_and_reset(s, s->formatter);
  }
}

void RGWListBucketMultiparts_ObjStore_S3::send_response()
{
  if (ret < 0)
    set_req_state_err(s, ret);
  dump_errno(s);

  end_header(s, "application/xml");
  dump_start(s);
  if (ret < 0)
    return;

  s->formatter->open_object_section("ListMultipartUploadsResult");
  s->formatter->dump_string("Bucket", s->bucket_name);
  if (!prefix.empty())
    s->formatter->dump_string("ListMultipartUploadsResult.Prefix", prefix);
  string& key_marker = marker.get_key();
  if (!key_marker.empty())
    s->formatter->dump_string("KeyMarker", key_marker);
  string& upload_id_marker = marker.get_upload_id();
  if (!upload_id_marker.empty())
    s->formatter->dump_string("UploadIdMarker", upload_id_marker);
  string next_key = next_marker.mp.get_key();
  if (!next_key.empty())
    s->formatter->dump_string("NextKeyMarker", next_key);
  string next_upload_id = next_marker.mp.get_upload_id();
  if (!next_upload_id.empty())
    s->formatter->dump_string("NextUploadIdMarker", next_upload_id);
  s->formatter->dump_int("MaxUploads", max_uploads);
  if (!delimiter.empty())
    s->formatter->dump_string("Delimiter", delimiter);
  s->formatter->dump_string("IsTruncated", (is_truncated ? "true" : "false"));

  if (ret >= 0) {
    vector<RGWMultipartUploadEntry>::iterator iter;
    for (iter = uploads.begin(); iter != uploads.end(); ++iter) {
      RGWMPObj& mp = iter->mp;
      s->formatter->open_array_section("Upload");
      s->formatter->dump_string("Key", mp.get_key());
      s->formatter->dump_string("UploadId", mp.get_upload_id());
      dump_owner(s, s->user.user_id, s->user.display_name, "Initiator");
      dump_owner(s, s->user.user_id, s->user.display_name);
      s->formatter->dump_string("StorageClass", "STANDARD");
      dump_time(s, "Initiated", &iter->obj.mtime);
      s->formatter->close_section();
    }
    if (common_prefixes.size() > 0) {
      s->formatter->open_array_section("CommonPrefixes");
      map<string, bool>::iterator pref_iter;
      for (pref_iter = common_prefixes.begin(); pref_iter != common_prefixes.end(); ++pref_iter) {
        s->formatter->dump_string("CommonPrefixes.Prefix", pref_iter->first);
      }
      s->formatter->close_section();
    }
  }
  s->formatter->close_section();
  rgw_flush_formatter_and_reset(s, s->formatter);
}

void RGWDeleteMultiObj_ObjStore_S3::send_status()
{
  if (!status_dumped) {
    if (ret < 0)
      set_req_state_err(s, ret);
    dump_errno(s);
    status_dumped = true;
  }
}

void RGWDeleteMultiObj_ObjStore_S3::begin_response()
{

  if (!status_dumped) {
    send_status();
  }

  dump_start(s);
  end_header(s, "application/xml");
  s->formatter->open_object_section_in_ns("DeleteResult",
                                            "http://s3.amazonaws.com/doc/2006-03-01/");

  rgw_flush_formatter(s, s->formatter);
}

void RGWDeleteMultiObj_ObjStore_S3::send_partial_response(pair<string,int>& result)
{
  if (!result.first.empty()) {
    if (result.second == 0 && !quiet) {
      s->formatter->open_object_section("Deleted");
      s->formatter->dump_string("Key", result.first);
      s->formatter->close_section();
    } else if (result.first < 0) {
      struct rgw_html_errors r;
      int err_no;

      s->formatter->open_object_section("Error");

      err_no = -(result.second);
      rgw_get_errno_s3(&r, err_no);

      s->formatter->dump_string("Key", result.first);
      s->formatter->dump_int("Code", r.http_ret);
      s->formatter->dump_string("Message", r.s3_code);
      s->formatter->close_section();
    }

    rgw_flush_formatter(s, s->formatter);
  }
}

void RGWDeleteMultiObj_ObjStore_S3::end_response()
{

  s->formatter->close_section();
  rgw_flush_formatter_and_reset(s, s->formatter);
}

RGWOp *RGWHandler_ObjStore_Service_S3::op_get()
{
  return new RGWListBuckets_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Service_S3::op_head()
{
  return new RGWListBuckets_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Bucket_S3::get_obj_op(bool get_data)
{
  if (get_data)
    return new RGWListBucket_ObjStore_S3;
  else
    return new RGWStatBucket_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Bucket_S3::op_get()
{
  if (s->args.sub_resource_exists("logging"))
    return new RGWGetBucketLogging_ObjStore_S3;
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_S3;
  } else if (s->args.exists("uploadId")) {
    return new RGWListMultipart_ObjStore_S3;
  }
  return get_obj_op(true);
}

RGWOp *RGWHandler_ObjStore_Bucket_S3::op_head()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_S3;
  } else if (s->args.exists("uploadId")) {
    return new RGWListMultipart_ObjStore_S3;
  }
  return get_obj_op(false);
}

RGWOp *RGWHandler_ObjStore_Bucket_S3::op_put()
{
  if (s->args.sub_resource_exists("logging"))
    return NULL;
  if (is_acl_op()) {
    return new RGWPutACLs_ObjStore_S3;
  }
  return new RGWCreateBucket_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Bucket_S3::op_delete()
{
  return new RGWDeleteBucket_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Bucket_S3::op_post()
{
  if ( s->request_params == "delete" ) {
    return new RGWDeleteMultiObj_ObjStore_S3;
  }

  return new RGWPostObj_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Obj_S3::get_obj_op(bool get_data)
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_S3;
  }
  RGWGetObj_ObjStore_S3 *get_obj_op = new RGWGetObj_ObjStore_S3;
  get_obj_op->set_get_data(get_data);
  return get_obj_op;
}

RGWOp *RGWHandler_ObjStore_Obj_S3::op_get()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_S3;
  } else if (s->args.exists("uploadId")) {
    return new RGWListMultipart_ObjStore_S3;
  }
  return get_obj_op(true);
}

RGWOp *RGWHandler_ObjStore_Obj_S3::op_head()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_S3;
  } else if (s->args.exists("uploadId")) {
    return new RGWListMultipart_ObjStore_S3;
  }
  return get_obj_op(false);
}

RGWOp *RGWHandler_ObjStore_Obj_S3::op_put()
{
  if (is_acl_op()) {
    return new RGWPutACLs_ObjStore_S3;
  }
  if (!s->copy_source)
    return new RGWPutObj_ObjStore_S3;
  else
    return new RGWCopyObj_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Obj_S3::op_delete()
{
  string upload_id = s->args.get("uploadId");

  if (upload_id.empty())
    return new RGWDeleteObj_ObjStore_S3;
  else
    return new RGWAbortMultipart_ObjStore_S3;
}

RGWOp *RGWHandler_ObjStore_Obj_S3::op_post()
{
  if (s->args.exists("uploadId"))
    return new RGWCompleteMultipart_ObjStore_S3;

  if (s->args.exists("uploads"))
    return new RGWInitMultipart_ObjStore_S3;

  return NULL;
}

int RGWHandler_ObjStore_S3::init_from_header(struct req_state *s, int default_formatter, bool configurable_format)
{
  string req;
  string first;

  const char *req_name = s->decoded_uri.c_str();
  const char *p;

  if (*req_name == '?') {
    p = req_name;
  } else {
    p = s->request_params.c_str();
  }

  s->args.set(p);
  s->args.parse();

  /* must be called after the args parsing */
  int ret = allocate_formatter(s, default_formatter, configurable_format);
  if (ret < 0)
    return ret;

  if (*req_name != '/')
    return 0;

  req_name++;

  if (!*req_name)
    return 0;

  req = req_name;
  int pos = req.find('/');
  if (pos >= 0) {
    first = req.substr(0, pos);
  } else {
    first = req;
  }

  if (!s->bucket_name) {
    s->bucket_name_str = first;
    s->bucket_name = strdup(s->bucket_name_str.c_str());

    if (pos >= 0) {
      string encoded_obj_str = req.substr(pos+1);
      s->object_str = encoded_obj_str;

      if (s->object_str.size() > 0) {
        s->object = strdup(s->object_str.c_str());
      }
    }
  } else {
    s->object_str = req_name;
    s->object = strdup(s->object_str.c_str());
  }
  return 0;
}

static bool looks_like_ip_address(const char *bucket)
{
  int num_periods = 0;
  bool expect_period = false;
  for (const char *b = bucket; *b; ++b) {
    if (*b == '.') {
      if (!expect_period)
	return false;
      ++num_periods;
      if (num_periods > 3)
	return false;
      expect_period = false;
    }
    else if (isdigit(*b)) {
      expect_period = true;
    }
    else {
      return false;
    }
  }
  return (num_periods == 3);
}

int RGWHandler_ObjStore_S3::validate_bucket_name(const string& bucket)
{
  int ret = RGWHandler_ObjStore::validate_bucket_name(bucket);
  if (ret < 0)
    return ret;

  if (bucket.size() == 0)
    return 0;

  if (!(isalpha(bucket[0]) || isdigit(bucket[0]))) {
    // bucket names must start with a number or letter
    return -ERR_INVALID_BUCKET_NAME;
  }

  for (const char *s = bucket.c_str(); *s; ++s) {
    char c = *s;
    if (isdigit(c) || (c == '.'))
      continue;
    if (isalpha(c))
      continue;
    if ((c == '-') || (c == '_'))
      continue;
    // Invalid character
    return -ERR_INVALID_BUCKET_NAME;
  }

  if (looks_like_ip_address(bucket.c_str()))
    return -ERR_INVALID_BUCKET_NAME;

  return 0;
}

int RGWHandler_ObjStore_S3::init(RGWRados *store, struct req_state *s, RGWClientIO *cio)
{
  dout(10) << "s->object=" << (s->object ? s->object : "<NULL>") << " s->bucket=" << (s->bucket_name ? s->bucket_name : "<NULL>") << dendl;

  int ret = validate_bucket_name(s->bucket_name_str);
  if (ret)
    return ret;
  ret = validate_object_name(s->object_str);
  if (ret)
    return ret;

  const char *cacl = s->env->get("HTTP_X_AMZ_ACL");
  if (cacl)
    s->canned_acl = cacl;

  s->copy_source = s->env->get("HTTP_X_AMZ_COPY_SOURCE");

  s->dialect = "s3";

  return RGWHandler_ObjStore::init(store, s, cio);
}

/*
 * ?get the canonical amazon-style header for something?
 */

static void get_canon_amz_hdr(struct req_state *s, string& dest)
{
  dest = "";
  map<string, string>::iterator iter;
  for (iter = s->x_meta_map.begin(); iter != s->x_meta_map.end(); ++iter) {
    dest.append(iter->first);
    dest.append(":");
    dest.append(iter->second);
    dest.append("\n");
  }
}

/*
 * ?get the canonical representation of the object's location
 */
static void get_canon_resource(struct req_state *s, string& dest)
{
  dest.append(s->request_uri.c_str());

  map<string, string>& sub = s->args.get_sub_resources();
  map<string, string>::iterator iter;
  for (iter = sub.begin(); iter != sub.end(); ++iter) {
    if (iter == sub.begin())
      dest.append("?");
    else
      dest.append("&");     
    dest.append(iter->first);
    if (!iter->second.empty()) {
      dest.append("=");
      dest.append(iter->second);
    }
  }
  dout(10) << "get_canon_resource(): dest=" << dest << dendl;
}

static inline bool is_base64_for_content_md5(unsigned char c) {
  return (isalnum(c) || isspace(c) || (c == '+') || (c == '/') || (c == '='));
}

/*
 * get the header authentication  information required to
 * compute a request's signature
 */
static bool get_auth_header(struct req_state *s, string& dest, bool qsr)
{
  dest = "";
  if (s->method)
    dest = s->method;
  dest.append("\n");
  
  const char *md5 = s->env->get("HTTP_CONTENT_MD5");
  if (md5) {
    for (const char *p = md5; *p; p++) {
      if (!is_base64_for_content_md5(*p)) {
        dout(0) << "NOTICE: bad content-md5 provided (not base64), aborting request p=" << *p << " " << (int)*p << dendl;
        return false;
      }
    }
    dest.append(md5);
  }
  dest.append("\n");

  const char *type = s->env->get("CONTENT_TYPE");
  if (type)
    dest.append(type);
  dest.append("\n");

  string date;
  if (qsr) {
    date = s->args.get("Expires");
  } else {
    const char *str = s->env->get("HTTP_DATE");
    const char *req_date = str;
    if (str) {
      date = str;
    } else {
      req_date = s->env->get("HTTP_X_AMZ_DATE");
      if (!req_date) {
        dout(0) << "NOTICE: missing date for auth header" << dendl;
        return false;
      }
    }

    struct tm t;
    if (!parse_rfc2616(req_date, &t)) {
      dout(0) << "NOTICE: failed to parse date for auth header" << dendl;
      return false;
    }
    if (t.tm_year < 70) {
      dout(0) << "NOTICE: bad date (predates epoch): " << req_date << dendl;
      return false;
    }
    s->header_time = utime_t(timegm(&t), 0);
  }

  if (date.size())
      dest.append(date);
  dest.append("\n");

  string canon_amz_hdr;
  get_canon_amz_hdr(s, canon_amz_hdr);
  dest.append(canon_amz_hdr);

  string canon_resource;
  get_canon_resource(s, canon_resource);
  dest.append(canon_resource);

  return true;
}

/*
 * verify that a signed request comes from the keyholder
 * by checking the signature against our locally-computed version
 */
int RGW_Auth_S3::authorize(RGWRados *store, struct req_state *s)
{
  bool qsr = false;
  string auth_id;
  string auth_sign;

  time_t now;
  time(&now);

  if (!s->http_auth || !(*s->http_auth)) {
    auth_id = s->args.get("AWSAccessKeyId");
    if (auth_id.size()) {
      auth_sign = s->args.get("Signature");

      string date = s->args.get("Expires");
      time_t exp = atoll(date.c_str());
      if (now >= exp)
        return -EPERM;

      qsr = true;
    } else {
      /* anonymous access */
      rgw_get_anon_user(s->user);
      s->perm_mask = RGW_PERM_FULL_CONTROL;
      return 0;
    }
  } else {
    if (strncmp(s->http_auth, "AWS ", 4))
      return -EINVAL;
    string auth_str(s->http_auth + 4);
    int pos = auth_str.find(':');
    if (pos < 0)
      return -EINVAL;

    auth_id = auth_str.substr(0, pos);
    auth_sign = auth_str.substr(pos + 1);
  }

  /* first get the user info */
  if (rgw_get_user_info_by_access_key(store, auth_id, s->user) < 0) {
    dout(5) << "error reading user info, uid=" << auth_id << " can't authenticate" << dendl;
    return -EPERM;
  }

  // populate the owner info
  s->owner.set_id(s->user.user_id);
  s->owner.set_name(s->user.display_name);

  /* now verify signature */
   
  string auth_hdr;
  if (!get_auth_header(s, auth_hdr, qsr)) {
    dout(10) << "failed to create auth header\n" << auth_hdr << dendl;
    return -EPERM;
  }
  dout(10) << "auth_hdr:\n" << auth_hdr << dendl;

  time_t req_sec = s->header_time.sec();
  if ((req_sec < now - RGW_AUTH_GRACE_MINS * 60 ||
      req_sec > now + RGW_AUTH_GRACE_MINS * 60) && !qsr) {
    dout(10) << "req_sec=" << req_sec << " now=" << now << "; now - RGW_AUTH_GRACE_MINS=" << now - RGW_AUTH_GRACE_MINS * 60 << "; now + RGW_AUTH_GRACE_MINS=" << now + RGW_AUTH_GRACE_MINS * 60 << dendl;
    dout(0) << "NOTICE: request time skew too big now=" << utime_t(now, 0) << " req_time=" << s->header_time << dendl;
    return -ERR_REQUEST_TIME_SKEWED;
  }

  map<string, RGWAccessKey>::iterator iter = s->user.access_keys.find(auth_id);
  if (iter == s->user.access_keys.end()) {
    dout(0) << "ERROR: access key not encoded in user info" << dendl;
    return -EPERM;
  }
  RGWAccessKey& k = iter->second;
  const char *key = k.key.c_str();
  int key_len = k.key.size();

  if (!k.subuser.empty()) {
    map<string, RGWSubUser>::iterator uiter = s->user.subusers.find(k.subuser);
    if (uiter == s->user.subusers.end()) {
      dout(0) << "NOTICE: could not find subuser: " << k.subuser << dendl;
      return -EPERM;
    }
    RGWSubUser& subuser = uiter->second;
    s->perm_mask = subuser.perm_mask;
  } else
    s->perm_mask = RGW_PERM_FULL_CONTROL;

  char hmac_sha1[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE];
  calc_hmac_sha1(key, key_len, auth_hdr.c_str(), auth_hdr.size(), hmac_sha1);

  char b64[64]; /* 64 is really enough */
  int ret = ceph_armor(b64, b64 + 64, hmac_sha1,
		       hmac_sha1 + CEPH_CRYPTO_HMACSHA1_DIGESTSIZE);
  if (ret < 0) {
    dout(10) << "ceph_armor failed" << dendl;
    return -EPERM;
  }
  b64[ret] = '\0';

  dout(15) << "b64=" << b64 << dendl;
  dout(15) << "auth_sign=" << auth_sign << dendl;
  dout(15) << "compare=" << auth_sign.compare(b64) << dendl;

  if (auth_sign.compare(b64) != 0)
    return -EPERM;

  return  0;
}

int RGWHandler_Auth_S3::init(RGWRados *store, struct req_state *state, RGWClientIO *cio)
{
  int ret = RGWHandler_ObjStore_S3::init_from_header(state, RGW_FORMAT_JSON, true);
  if (ret < 0)
    return ret;

  return RGWHandler_ObjStore::init(store, state, cio);
}

RGWHandler *RGWRESTMgr_S3::get_handler(struct req_state *s)
{
  int ret = RGWHandler_ObjStore_S3::init_from_header(s, RGW_FORMAT_XML, false);
  if (ret < 0)
    return NULL;

  if (!s->bucket_name)
    return new RGWHandler_ObjStore_Service_S3;

  if (!s->object)
    return new RGWHandler_ObjStore_Bucket_S3;

  return new RGWHandler_ObjStore_Obj_S3;
}
