#include "file_naming.h"

fs::path MakeImagePath(const fs::path &folder, const fs::path &prefix, long long id)
{
	fs::path name = prefix;
	if (!name.empty())
		name += "_";
	name += std::to_string(id);
	name += ".png";
	return folder / name;
}

long long ScanMaxIndex(const fs::path &folder, const fs::path &prefix)
{
	fs::path::string_type pfx;
	if (!prefix.empty())
	{
		fs::path p = prefix;
		p += "_";
		pfx = p.native();
	}

	long long maxId = -1;
	std::error_code ec;
	fs::directory_iterator it(folder, ec), end;
	for (; !ec && it != end; it.increment(ec))
	{
		std::error_code ec2;
		if (!it->is_regular_file(ec2))
			continue;

		const fs::path &p = it->path();
		const fs::path::string_type ext = p.extension().native();
		if (!(ext.size() == 4 && ext[0] == '.' && (ext[1] | 0x20) == 'p' && (ext[2] | 0x20) == 'n' && (ext[3] | 0x20) == 'g'))
			continue;

		const fs::path::string_type stem = p.stem().native();
		if (stem.size() <= pfx.size())
			continue;
		if (!pfx.empty() && stem.compare(0, pfx.size(), pfx) != 0)
			continue;

		long long id = 0;
		bool ok = (stem.size() - pfx.size()) <= 9; // 最多 9 位，避免溢出
		for (size_t i = pfx.size(); ok && i < stem.size(); ++i)
		{
			if (stem[i] < '0' || stem[i] > '9')
				ok = false;
			else
				id = id * 10 + (stem[i] - '0');
		}
		if (ok && id > maxId)
			maxId = id;
	}
	return maxId;
}
