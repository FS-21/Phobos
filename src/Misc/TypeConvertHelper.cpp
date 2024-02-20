#include "Misc/TypeConvertHelper.h"

void TypeConvertHelper::Convert(FootClass* pTargetFoot, const std::vector<TypeConvertGroup>& convertPairs, HouseClass* pOwner, AnimTypeClass* pTypeAnim = nullptr)
{
	for (const auto& [fromTypes, toType, affectedHouses] : convertPairs)
	{
		if (!toType.isset() || !toType.Get()) continue;

		if (!EnumFunctions::CanTargetHouse(affectedHouses, pOwner, pTargetFoot->Owner))
			continue;

		if (fromTypes.size())
		{
			for (const auto& from : fromTypes)
			{
				// Check if the target matches upgrade-from TechnoType and it has something to upgrade to
				if (from == pTargetFoot->GetTechnoType())
				{
					bool converted = TechnoExt::ConvertToType(pTargetFoot, toType);

					if (converted && pTypeAnim)
					{
						if (auto pAnim = GameCreate<AnimClass>(pTypeAnim, pTargetFoot->Location))
							pAnim->SetOwnerObject(pTargetFoot);
					}

					break;
				}
			}
		}
		else
		{
			TechnoExt::ConvertToType(pTargetFoot, toType);
		}
	}
}

void TypeConvertHelper::UniversalConvert(TechnoClass* pTarget, const std::vector<TypeConvertGroup>& convertPairs, HouseClass* pOwner, AnimTypeClass* pTypeAnim = nullptr)
{
	for (const auto& [fromTypes, toType, affectedHouses] : convertPairs)
	{
		if (!toType.isset() || !toType.Get()) continue;

		if (!EnumFunctions::CanTargetHouse(affectedHouses, pOwner, pTarget->Owner))
			continue;

		if (fromTypes.size())
		{
			for (const auto& from : fromTypes)
			{
				// Check if the target matches upgrade-from TechnoType and it has something to upgrade to
				if (from == pTarget->GetTechnoType())
				{
					if (pTarget->Target)
					{
						auto pTargetExt = TechnoExt::ExtMap.Find(pTarget);
						pTargetExt->Convert_UniversalDeploy_RememberTarget = pTarget->Target;
					}

					bool converted = TechnoExt::UniversalDeployConversion(pTarget, toType) ? true : false;

					if (converted && pTypeAnim)
					{
						if (auto pAnim = GameCreate<AnimClass>(pTypeAnim, pTarget->Location))
							pAnim->SetOwnerObject(pTarget);
					}

					break;
				}
			}
		}
		else
		{
			TechnoExt::UniversalDeployConversion(pTarget, toType);
		}
	}
}

bool TypeConvertGroup::Load(PhobosStreamReader& stm, bool registerForChange)
{
	return this->Serialize(stm);
}

bool TypeConvertGroup::Save(PhobosStreamWriter& stm) const
{
	return const_cast<TypeConvertGroup*>(this)->Serialize(stm);
}

template <typename T>
bool TypeConvertGroup::Serialize(T& stm)
{
	return stm
		.Process(this->FromTypes)
		.Process(this->ToType)
		.Process(this->AppliedTo)
		.Success();
}
