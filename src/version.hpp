#pragma once

namespace big
{
	struct version
	{
		static const char* GIT_SHA1;
		static const char* GIT_DATE;
		static const char* GIT_COMMIT_SUBJECT;
		static const char* GIT_BRANCH;

		// .github/workflows/release.yml rewrites this line from the git tag.
		// Keep the `VERSION_NUMBER = "x.y.z"` shape or the sed misses.
		static inline const char* VERSION_NUMBER = "0.3.0";
	};
}
