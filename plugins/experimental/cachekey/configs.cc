/*
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/**
 * @file configs.cc
 * @brief Plugin configuration.
 */

#include <fstream>   /* std::ifstream */
#include <sstream>   /* std::istringstream */
#include <getopt.h>  /* getopt_long() */
#include <strings.h> /* strncasecmp() */

#include "configs.h"

template <typename ContainerType>
static void
commaSeparateString(ContainerType &c, const String &input)
{
  std::istringstream istr(input);
  String token;

  while (std::getline(istr, token, ',')) {
    c.insert(c.end(), token);
  }
}

static bool
isTrue(const char *arg)
{
  return (0 == strncasecmp("true", arg, 4) || 0 == strncasecmp("1", arg, 1) || 0 == strncasecmp("yes", arg, 3));
}

void
ConfigElements::setExclude(const char *arg)
{
  ::commaSeparateString<StringSet>(_exclude, arg);
}

void
ConfigElements::setInclude(const char *arg)
{
  ::commaSeparateString<StringSet>(_include, arg);
}

static void
setPattern(MultiPattern &multiPattern, const char *arg)
{
  Pattern *p = new Pattern();
  if (NULL != p && p->init(arg)) {
    multiPattern.add(p);
  } else {
    delete p;
  }
}

void
ConfigElements::setExcludePatterns(const char *arg)
{
  setPattern(_excludePatterns, arg);
}

void
ConfigElements::setIncludePatterns(const char *arg)
{
  setPattern(_includePatterns, arg);
}

void
ConfigElements::setSort(const char *arg)
{
  _sort = ::isTrue(arg);
}

void
ConfigElements::setRemove(const char *arg)
{
  _remove = ::isTrue(arg);
}

bool
ConfigElements::toBeRemoved() const
{
  return _remove;
}

bool
ConfigElements::toBeSkipped() const
{
  return _skip;
}

bool
ConfigElements::toBeSorted() const
{
  return _sort;
}

bool
ConfigElements::toBeAdded(const String &element) const
{
  /* Exclude the element if it is in the exclusion list. If the list is empty don't exclude anything. */
  bool exclude = (!_exclude.empty() && _exclude.find(element) != _exclude.end()) ||
                 (!_excludePatterns.empty() && _excludePatterns.match(element));
  CacheKeyDebug("%s '%s' %s the 'exclude' rule", name().c_str(), element.c_str(), exclude ? "matches" : "does not match");

  /* Include the element only if it is in the inclusion list. If the list is empty include everything. */
  bool include =
    ((_include.empty() && _includePatterns.empty()) || _include.find(element) != _include.end()) || _includePatterns.match(element);
  CacheKeyDebug("%s '%s' %s the 'include' rule", name().c_str(), element.c_str(), include ? "matches" : "do not match");

  if (include && !exclude) {
    CacheKeyDebug("%s '%s' should be added to cache key", name().c_str(), element.c_str());
    return true;
  }

  CacheKeyDebug("%s '%s' should not be added to cache key", name().c_str(), element.c_str());
  return false;
}

inline bool
ConfigElements::noIncludeExcludeRules() const
{
  return _exclude.empty() && _excludePatterns.empty() && _include.empty() && _includePatterns.empty();
}

/**
 * @brief finalizes the query parameters related configuration.
 *
 * If we don't have any inclusions or exclusions and don't have to sort, we don't need to do anything
 * with the query string. Include the whole original query in the cache key.
 */
bool
ConfigQuery::finalize()
{
  _skip = noIncludeExcludeRules() && !_sort;
  return true;
}

const String ConfigQuery::_NAME = "query parameter";
inline const String &
ConfigQuery::name() const
{
  return _NAME;
}

/**
 * @briefs finalizes the headers related configuration.
 *
 * If the all include and exclude lists are empty, including patterns, then there is no headers to be included.
 */
bool
ConfigHeaders::finalize()
{
  _remove = noIncludeExcludeRules();
  return true;
}

const String ConfigHeaders::_NAME = "header";
inline const String &
ConfigHeaders::name() const
{
  return _NAME;
}

/**
 * @brief finalizes the cookies related configuration.
 *
 * If the all include and exclude lists are empty, including pattern, then there is no cookies to be included.
 */
bool
ConfigCookies::finalize()
{
  _remove = noIncludeExcludeRules();
  return true;
}

const String ConfigCookies::_NAME = "cookie";
inline const String &
ConfigCookies::name() const
{
  return _NAME;
}

/**
 * @brief Accessor method for getting include list only for headers config.
 *
 * We would not need to drill this hole in the design if there was an efficient way to iterate through the headers in the traffic
 * server API (inefficiency mentioned in ts/ts.h), iterating through the "include" list should be good enough work-around.
 */
const StringSet &
ConfigHeaders::getInclude() const
{
  return _include;
}

/**
 * @brief Rebase a relative path onto the configuration directory.
 */
static String
makeConfigPath(const String &path)
{
  if (path.empty() || path[0] == '/') {
    return path;
  }

  return String(TSConfigDirGet()) + "/" + path;
}

/**
 * @brief a helper function which loads the classifier from files.
 * @param args classname + filename in '<classname>:<filename>' format.
 * @param blacklist true - load as a blacklist classifier, false - whitelist.
 * @return true if successful, false otherwise.
 */
bool
Configs::loadClassifiers(const String &args, bool blacklist)
{
  static const char *EXPECTED_FORMAT = "<classname>:<filename>";

  std::size_t d = args.find(':');
  if (String::npos == d) {
    CacheKeyError("failed to parse classifier string '%s', expected format: '%s'", optarg ? optarg : "null", EXPECTED_FORMAT);
    return false;
  }

  String classname(optarg, 0, d);
  String filename(optarg, d + 1, String::npos);

  if (classname.empty() || filename.empty()) {
    CacheKeyError("'<classname>' and '<filename>' in '%s' cannot be empty, expected format: '%s'", optarg ? optarg : "null",
                  EXPECTED_FORMAT);
    return false;
  }

  String path(makeConfigPath(filename));

  std::ifstream ifstr;
  String regex;
  unsigned lineno = 0;

  ifstr.open(path.c_str());
  if (!ifstr) {
    CacheKeyError("failed to load classifier '%s' from '%s'", classname.c_str(), path.c_str());
    return false;
  }

  MultiPattern *multiPattern;
  if (blacklist) {
    multiPattern = new NonMatchingMultiPattern(classname);
  } else {
    multiPattern = new MultiPattern(classname);
  }
  if (NULL == multiPattern) {
    CacheKeyError("failed to allocate classifier '%s'", classname.c_str());
    return false;
  }

  CacheKeyDebug("loading classifier '%s' from '%s'", classname.c_str(), path.c_str());

  while (std::getline(ifstr, regex)) {
    Pattern *p;
    String::size_type pos;

    ++lineno;

    // Allow #-prefixed comments.
    pos = regex.find_first_of('#');
    if (pos != String::npos) {
      regex.resize(pos);
    }

    if (regex.empty()) {
      continue;
    }

    p = new Pattern();

    if (NULL != p && p->init(regex)) {
      if (blacklist) {
        CacheKeyDebug("Added pattern '%s' to black list '%s'", regex.c_str(), classname.c_str());
        multiPattern->add(p);
      } else {
        CacheKeyDebug("Added pattern '%s' to white list '%s'", regex.c_str(), classname.c_str());
        multiPattern->add(p);
      }
    } else {
      CacheKeyError("%s:%u: failed to parse regex '%s'", path.c_str(), lineno, regex.c_str());
      delete p;
    }
  }

  ifstr.close();

  if (!multiPattern->empty()) {
    _classifier.add(multiPattern);
  } else {
    delete multiPattern;
  }

  return true;
}

/**
 * @brief initializes plugin configuration.
 * @param argc number of plugin parameters
 * @param argv plugin parameters
 */
bool
Configs::init(int argc, char *argv[])
{
  static const struct option longopt[] = {{"exclude-params", optional_argument, 0, 'a'},
                                          {"include-params", optional_argument, 0, 'b'},
                                          {"include-match-params", optional_argument, 0, 'c'},
                                          {"exclude-match-params", optional_argument, 0, 'd'},
                                          {"sort-params", optional_argument, 0, 'e'},
                                          {"remove-all-params", optional_argument, 0, 'f'},
                                          {"include-headers", optional_argument, 0, 'g'},
                                          {"include-cookies", optional_argument, 0, 'h'},
                                          {"ua-capture", optional_argument, 0, 'i'},
                                          {"static-prefix", optional_argument, 0, 'j'},
                                          {"capture-prefix", optional_argument, 0, 'k'},
                                          {"ua-whitelist", optional_argument, 0, 'l'},
                                          {"ua-blacklist", optional_argument, 0, 'm'},
                                          {0, 0, 0, 0}};

  bool status = true;
  optind = 0;

  /* argv contains the "to" and "from" URLs. Skip the first so that the second one poses as the program name. */
  argc--;
  argv++;

  for (;;) {
    int opt;
    opt = getopt_long(argc, (char *const *)argv, "", longopt, NULL);

    if (opt == -1) {
      break;
    }
    CacheKeyDebug("processing %s", argv[optind - 1]);

    switch (opt) {
    case 'a': /* exclude-params */
      _query.setExclude(optarg);
      break;
    case 'b': /* include-params */
      _query.setInclude(optarg);
      break;
    case 'c': /* include-match-params */
      _query.setIncludePatterns(optarg);
      break;
    case 'd': /* exclude-match-params */
      _query.setExcludePatterns(optarg);
      break;
    case 'e': /* sort-params */
      _query.setSort(optarg);
      break;
    case 'f': /* remove-all-params */
      _query.setRemove(optarg);
      break;
    case 'g': /* include-headers */
      _headers.setInclude(optarg);
      break;
    case 'h': /* include-cookies */
      _cookies.setInclude(optarg);
      break;
    case 'i': /* ua-capture */
      if (!_uaCapture.init(optarg)) {
        CacheKeyError("failed to initialize User-Agent capture pattern '%s'", optarg);
        status = false;
      }
      break;
    case 'j': /* static-prefix */
      _prefix.assign(optarg);
      CacheKeyDebug("prefix='%s'", _prefix.c_str());
      break;
    case 'k': /* capture-prefix */
      if (!_hostCapture.init(optarg)) {
        CacheKeyError("failed to initialize URI host:port capture pattern '%s'", optarg);
        status = false;
      }
      break;
    case 'l': /* ua-whitelist */
      if (!loadClassifiers(optarg, /* blacklist = */ false)) {
        CacheKeyError("failed to load User-Agent pattern white-list '%s'", optarg);
        status = false;
      }
      break;
    case 'm': /* ua-blacklist */
      if (!loadClassifiers(optarg, /* blacklist = */ true)) {
        CacheKeyError("failed to load User-Agent pattern black-list '%s'", optarg);
        status = false;
      }
      break;
    }
  }

  status &= finalize();

  return status;
}

/**
 * @brief provides means for post-processing of the plugin parameters to finalize the configuration or to "cache" some of the
 * decisions for later use.
 * @return true if successful, false if failure.
 */
bool
Configs::finalize()
{
  return _query.finalize() && _headers.finalize() && _cookies.finalize();
}
