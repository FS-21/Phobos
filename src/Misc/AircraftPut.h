#pragma once

#include <MapClass.h>
#include <AircraftClass.h>
#include <AircraftTypeClass.h>

#include <Helpers/Macro.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>
#include <Utilities/Container.h>

// Untested

struct AircraftPutData
{
public:
	Nullable<CoordStruct> PutOffset;
	Valueable<bool> Force;
	Valueable<bool> RemoveIfNoDock;

	bool operator==(const AircraftPutData& that) const
	{
		return false;
	}

	operator bool() const
	{
		return PutOffset.isset();
	}

	static const void LoadFromINI(AircraftPutData &nDiveData, INI_EX &parser, const char* pSection)
	{
		if (!pSection)
			return;

		nDiveData.PutOffset.Read(parser, pSection, "NoHelipadPutOffset");
		nDiveData.Force.Read(parser, pSection, "ForcePutOffset");
		nDiveData.RemoveIfNoDock.Read(parser, pSection, "RemoveIfNoDocks");
	}

	AircraftPutData() noexcept :

		PutOffset(),
		Force(false),
		RemoveIfNoDock(false)
	{}

	explicit AircraftPutData(noinit_t) noexcept
	{ }

};
