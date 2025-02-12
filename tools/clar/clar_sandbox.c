/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

static char _clar_path[4096];

static int
is_valid_tmp_path(const char *path)
{
	STAT_T st;

	if (stat(path, &st) != 0)
		return 0;

	if (!S_ISDIR(st.st_mode))
		return 0;

	return (access(path, W_OK) == 0);
}

static int
find_tmp_path(char *buffer, size_t length)
{
#ifndef _WIN32
	static const size_t var_count = 4;
	static const char *env_vars[] = {
		"TMPDIR", "TMP", "TEMP", "USERPROFILE"
 	};

 	size_t i;

	for (i = 0; i < var_count; ++i) {
		const char *env = getenv(env_vars[i]);
		if (!env)
			continue;

		if (is_valid_tmp_path(env)) {
			strncpy(buffer, env, length);
			return 0;
		}
	}

	/* If the environment doesn't say anything, try to use /tmp */
	if (is_valid_tmp_path("/tmp")) {
		strncpy(buffer, "/tmp", length);
		return 0;
	}

#else
	if (GetTempPath((DWORD)length, buffer))
		return 0;
#endif

	/* This system doesn't like us, try to use the current directory */
	if (is_valid_tmp_path(".")) {
		strncpy(buffer, ".", length);
		return 0;
	}

	return -1;
}

static void clar_unsandbox(void)
{
	if (_clar_path[0] == '\0')
		return;

#ifdef _WIN32
	chdir("..");
#endif

	fs_rm(_clar_path);
}

char *mkdtemp(char *template);

static int build_sandbox_path(void)
{
	const char path_tail[] = "clar_tmp_XXXXXX";
	size_t len;

	if (find_tmp_path(_clar_path, sizeof(_clar_path)) < 0)
		return -1;

	len = strlen(_clar_path);

#ifdef _WIN32
	{ /* normalize path to POSIX forward slashes */
		size_t i;
		for (i = 0; i < len; ++i) {
			if (_clar_path[i] == '\\')
				_clar_path[i] = '/';
		}
	}
#endif

	if (_clar_path[len - 1] != '/') {
		_clar_path[len++] = '/';
	}

	strncpy(_clar_path + len, path_tail, sizeof(_clar_path) - len);

#if defined(__MINGW32__)
	if (_mktemp(_clar_path) == NULL)
		return -1;

	if (mkdir(_clar_path, 0700) != 0)
		return -1;
#elif defined(_WIN32)
	if (_mktemp_s(_clar_path, sizeof(_clar_path)) != 0)
		return -1;

	if (mkdir(_clar_path, 0700) != 0)
		return -1;
#else
	if (mkdtemp(_clar_path) == NULL)
		return -1;
#endif

	return 0;
}

static int clar_sandbox(void)
{
	if (_clar_path[0] == '\0' && build_sandbox_path() < 0)
		return -1;

	if (chdir(_clar_path) != 0)
		return -1;

	return 0;
}

