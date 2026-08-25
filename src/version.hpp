#pragma once

namespace big
{
	struct version
	{
		static const char* GIT_SHA1;
		static const char* GIT_DATE;
		static const char* GIT_COMMIT_SUBJECT;
		static const char* GIT_BRANCH;

		// Nightly CI rewrites this line, deriving the value from the current
		// published Thunderstore version and bumping the patch. Keep the
		// `VERSION_NUMBER = "x.y.z"` shape or the sed in nightly.yml misses.
		static inline const char* VERSION_NUMBER = "1.0.0";
	};
}
