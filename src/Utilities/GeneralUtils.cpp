#include "Constructs.h"
#include "GeneralUtils.h"
#include <AStarClass.h>
#include "Debug.h"
#include <Theater.h>
#include <BitFont.h>

#include <Ext/Rules/Body.h>
#include <Ext/Techno/Body.h>
#include <Misc/FlyingStrings.h>
#include "AresHelper.h"

bool GeneralUtils::IsValidString(const char* str)
{
	return str != nullptr
		&& strlen(str) != 0
		&& !INIClass::IsBlank(str);
}

void GeneralUtils::IntValidCheck(int* source, const char* section, const char* tag, int defaultValue, int min, int max)
{
	if (*source < min || *source>max)
	{
		Debug::Log("[Developer warning][%s]%s=%d is invalid! Reset to %d.\n", section, tag, *source, defaultValue);
		*source = defaultValue;
	}
}

void GeneralUtils::DoubleValidCheck(double* source, const char* section, const char* tag, double defaultValue, double min, double max)
{
	if (*source < min || *source>max)
	{
		Debug::Log("[Developer warning][%s]%s=%f is invalid! Reset to %f.\n", section, tag, *source, defaultValue);
		*source = defaultValue;
	}
}

const wchar_t* GeneralUtils::LoadStringOrDefault(const char* key, const wchar_t* defaultValue)
{
	if (GeneralUtils::IsValidString(key))
		return StringTable::LoadString(key);
	else
		return defaultValue;
}

const wchar_t* GeneralUtils::LoadStringUnlessMissing(const char* key, const wchar_t* defaultValue)
{
	return wcsstr(LoadStringOrDefault(key, defaultValue), L"MISSING:") ? defaultValue : LoadStringOrDefault(key, defaultValue);
}

std::vector<CellStruct> GeneralUtils::AdjacentCellsInRange(unsigned int range)
{
	std::vector<CellStruct> result;
	result.reserve((2 * range + 1) * (2 * range + 1));

	for (CellSpreadEnumerator it(range); it; ++it)
		result.push_back(*it);

	return result;
}

const int GeneralUtils::GetRangedRandomOrSingleValue(PartialVector2D<int> range)
{
	return range.X >= range.Y || range.ValueCount < 2 ? range.X : ScenarioClass::Instance->Random.RandomRanged(range.X, range.Y);
}

const double GeneralUtils::GetRangedRandomOrSingleValue(PartialVector2D<double> range)
{
	const int min = static_cast<int>(range.X * 100);
	const int max = static_cast<int>(range.Y * 100);

	return range.X >= range.Y || range.ValueCount < 2 ? range.X : (ScenarioClass::Instance->Random.RandomRanged(min, max) / 100.0);
}

struct VersesData
{
	double Verses;
	WarheadFlags Flags;
};

struct DummyTypeExtHere
{
	char _[0x24];
	std::vector<VersesData> Verses;
};

const double GeneralUtils::GetWarheadVersusArmor(WarheadTypeClass* pWH, Armor armorType)
{
	if (AresHelper::CanUseAres)
		return reinterpret_cast<DummyTypeExtHere*>(*(uintptr_t*)((char*)pWH + 0x1CC))->Verses[static_cast<int>(armorType)].Verses;

	return static_cast<double>(MapClass::GetTotalDamage(100, pWH, armorType, 0)) / 100.0;
}

const double GeneralUtils::GetWarheadVersusArmor(WarheadTypeClass* pWH, TechnoClass* pThis, TechnoTypeClass* pType)
{
	auto armorType = pType->Armor;
	auto const pShield = TechnoExt::ExtMap.Find(pThis)->Shield.get();

	if (pShield && pShield->IsActive() && !pShield->CanBePenetrated(pWH))
		armorType = pShield->GetArmorType(pType);

	return GeneralUtils::GetWarheadVersusArmor(pWH, armorType);
}

// Weighted random element choice (weight) - roll for one.
// Takes a vector of integer type weights, which are then summed to calculate the chances.
// Returns chosen index or -1 if nothing is chosen.
int GeneralUtils::ChooseOneWeighted(const double dice, const std::vector<int>* weights)
{
	float sum = 0.0;
	float sum2 = 0.0;

	for (size_t i = 0; i < weights->size(); i++)
		sum += (*weights)[i];

	for (size_t i = 0; i < weights->size(); i++)
	{
		sum2 += (*weights)[i];
		if (dice < (sum2 / sum))
			return i;
	}

	return -1;
}

// Checks if health ratio has changed threshold (Healthy/ConditionYellow/Red).
bool GeneralUtils::HasHealthRatioThresholdChanged(double oldRatio, double newRatio)
{
	if (oldRatio == newRatio)
		return false;

	if (oldRatio > RulesClass::Instance->ConditionYellow
		&& newRatio <= RulesClass::Instance->ConditionYellow)
	{
		return true;
	}
	else if (oldRatio <= RulesClass::Instance->ConditionYellow
		&& oldRatio > RulesClass::Instance->ConditionRed
		&& (newRatio <= RulesClass::Instance->ConditionRed || newRatio > RulesClass::Instance->ConditionYellow))
	{
		return true;
	}
	else if (oldRatio <= RulesClass::Instance->ConditionRed
		&& newRatio > RulesClass::Instance->ConditionRed)
	{
		return true;
	}

	return false;
}

bool GeneralUtils::ApplyTheaterSuffixToString(char* str)
{
	if (auto pSuffix = strstr(str, "~~~"))
	{
		const auto theater = ScenarioClass::Instance->Theater;
		const auto pExtension = Theater::GetTheater(theater).Extension;
		pSuffix[0] = pExtension[0];
		pSuffix[1] = pExtension[1];
		pSuffix[2] = pExtension[2];
		return true;
	}

	return false;
}

std::string GeneralUtils::IntToDigits(int num)
{
	std::string digits;
	digits.reserve(10); // 32-bit int max: 2,147,483,647 (10 digits)

	if (num == 0)
	{
		digits.push_back('0');
		return digits;
	}

	while (num)
	{
		digits.push_back(static_cast<char>(num % 10) + '0');
		num /= 10;
	}

	std::reverse(digits.begin(), digits.end());

	return digits;
}

int GeneralUtils::CountDigitsInNumber(int number)
{
	int digits = 0;

	while (number)
	{
		number /= 10;
		digits++;
	}

	return digits;
}

// Calculates a new coordinates based on current & target coordinates within specified distance (can be negative to switch the direction) in leptons.
CoordStruct GeneralUtils::CalculateCoordsFromDistance(CoordStruct currentCoords, CoordStruct targetCoords, int distance)
{
	const int deltaX = currentCoords.X - targetCoords.X;
	const int deltaY = targetCoords.Y - currentCoords.Y;

	const double atan = Math::atan2(deltaY, deltaX);
	const double radians = (((atan - Math::HalfPi) * (1.0 / Math::GameDegreesToRadiansCoefficient)) - Math::GameDegrees90) * Math::GameDegreesToRadiansCoefficient;
	const int x = static_cast<int>(targetCoords.X + Math::cos(radians) * distance);
	const int y = static_cast<int>(targetCoords.Y - Math::sin(radians) * distance);

	return CoordStruct { x, y, targetCoords.Z };
}

void GeneralUtils::DisplayDamageNumberString(int damage, DamageDisplayType type, CoordStruct coords, int& offset)
{
	if (damage == 0)
		return;

	ColorStruct color;

	switch (type)
	{
	case DamageDisplayType::Regular:
		color = damage > 0 ? ColorStruct { 255, 0, 0 } : ColorStruct { 0, 255, 0 };
		break;
	case DamageDisplayType::Shield:
		color = damage > 0 ? ColorStruct { 0, 160, 255 } : ColorStruct { 0, 255, 230 };
		break;
	case DamageDisplayType::Intercept:
		color = damage > 0 ? ColorStruct { 255, 128, 128 } : ColorStruct { 128, 255, 128 };
		break;
	default:
		break;
	}

	const int maxOffset = Unsorted::CellWidthInPixels / 2;
	int width = 0, height = 0;
	wchar_t damageStr[0x20];
	swprintf_s(damageStr, L"%d", damage);

	BitFont::Instance->GetTextDimension(damageStr, &width, &height, 120);

	if (offset >= maxOffset || offset == INT32_MIN)
		offset = -maxOffset;

	FlyingStrings::Add(damageStr, coords, color, Point2D { offset - (width / 2), 0 });

	offset = offset + width;
}

DynamicVectorClass<ColorScheme*>* GeneralUtils::BuildPalette(const char* paletteFileName)
{
	if (GeneralUtils::IsValidString(paletteFileName))
	{
		char pFilename[0x20];
		strcpy_s(pFilename, paletteFileName);

		return ColorScheme::GeneratePalette(pFilename);
	}

	return nullptr;
}

// Gets integer representation of color from ColorAdd corresponding to given index, or 0 if there's no color found.
// Code is pulled straight from game's draw functions that deal with the tint colors.
int GeneralUtils::GetColorFromColorAdd(int colorIndex)
{
	auto const& colorAdd = RulesClass::Instance->ColorAdd;
	int colorValue = 0;

	if (colorIndex < 0 || colorIndex >= (sizeof(colorAdd) / sizeof(ColorStruct)))
		return colorValue;

	auto const& color = colorAdd[colorIndex];

	if (RulesExt::Global()->ColorAddUse8BitRGB)
		return Drawing::RGB_To_Int(color);

	const int red = color.R;
	const int green = color.G;
	const int blue = color.B;

	switch (Drawing::ColorMode)
	{
	case RGBMode::RGB565:
		colorValue |= (red << 6 | green) << 5 | blue;
		break;
	case RGBMode::RGB556:
		colorValue |= (red << 5 | green >> 1) << 6 | blue;
		break;
	default:
		colorValue |= (red << 5 | green >> 1) << 5 | blue;
		break;
	}

	return colorValue;
}

int GeneralUtils::SafeMultiply(int value, int mult)
{
	long long product = static_cast<long long>(value) * mult;

	if (product > INT32_MAX)
		product = INT32_MAX;
	else if (product < INT32_MIN)
		product = INT32_MIN;

	return static_cast<int>(product);
}

int GeneralUtils::SafeMultiply(int value, double mult)
{
	double product = static_cast<double>(value) * mult;

	if (product > INT32_MAX)
		product = INT32_MAX;
	else if (product < INT32_MIN)
		product = INT32_MIN;

	return static_cast<int>(product);
}



static bool IsCellBlocked(CellStruct coords)
{
	if (const CellClass* cell = MapClass::Instance.GetCellAt(coords))
	{
		if (cell->GetBuilding() != nullptr)
			return true;
		if (cell->GetTerrain(false) != nullptr)
			return true;
		if (cell->Tile_Is_Cliff() || cell->Tile_Is_Water())
			return true;
	}
	return false;
}

static CellStruct GetPassableNeighbor(CellStruct center)
{
	for (int r = 1; r <= 3; r++)
	{
		for (int dy = -r; dy <= r; dy++)
		{
			for (int dx = -r; dx <= r; dx++)
			{
				CellStruct testCell(center.X + dx, center.Y + dy);
				if (MapClass::Instance.CoordinatesLegal(testCell))
				{
					const CellClass* cell = MapClass::Instance.GetCellAt(testCell);
					if (cell)
					{
						if (cell->GetBuilding() == nullptr && cell->GetTerrain(false) == nullptr && !cell->Tile_Is_Cliff() && !cell->Tile_Is_Water())
						{
							return testCell;
						}
					}
				}
			}
		}
	}
	return center;
}

static FootClass* GetRepresentativeFootForCell(CellStruct cellCoords)
{
	if (const CellClass* cell = MapClass::Instance.GetCellAt(cellCoords))
	{
		if (const BuildingClass* pBld = cell->GetBuilding())
		{
			HouseClass* pHouse = pBld->Owner;
			if (pHouse)
			{
				for (const auto pUnit : UnitClass::Array)
				{
					if (pUnit && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Owner == pHouse && !pUnit->Type->Naval)
					{
						return static_cast<FootClass*>(pUnit);
					}
				}
				for (const auto pInf : InfantryClass::Array)
				{
					if (pInf && pInf->IsAlive && !pInf->InLimbo && pInf->Owner == pHouse)
					{
						return static_cast<FootClass*>(pInf);
					}
				}
			}
		}
	}
	return nullptr;
}

int GeneralUtils::GetAStarPathLength(CellStruct fromCell, CellStruct toCell, MovementZone movementZone)
{
	FootClass* pFoot = GetRepresentativeFootForCell(fromCell);

	CellStruct start = fromCell;
	if (IsCellBlocked(fromCell))
		start = GetPassableNeighbor(fromCell);

	CellStruct end = toCell;
	if (IsCellBlocked(toCell))
		end = GetPassableNeighbor(toCell);

	int res = AStarClass::Instance.AttemptPath(&start, &end, pFoot, false, false, movementZone);
	
	// Only log path failures
	/*
	if (res <= 0 || res > 100000)
	{
		Debug::Log("Phobos Pathfinder: (%d,%d) [adj (%d,%d)] -> (%d,%d) [adj (%d,%d)] (zone %d, foot %s) -> Result: %d\n",
			fromCell.X, fromCell.Y, start.X, start.Y, toCell.X, toCell.Y, end.X, end.Y, static_cast<int>(movementZone),
			pFoot ? pFoot->GetTechnoType()->ID : "nullptr", res);
	}
	*/
	
	return res;
}

// Checks if two map cells are connected by land (passable by ground units).
//
// WHY NOT USE NATIVE ENGINE CHECKS?
// 1. Structure-to-structure 'IsInSameZoneAs' is double-bugged:
//    - Returns YES across water/islands (because structures have null/0 movement zones, causing a 0==0 match).
//    - Returns NO on land maps when bases are walled (because structures do not bypass walls/fences).
// 2. Unit-to-coordinate 'IsInSameZoneAsCoords' is stateful and inconsistent:
//    - It depends on the unit's current physical position. If a friendly unit is dropped
//      or spawned at toCell, it would incorrectly report a connection between fromCell and toCell.
//
// WHY USE ASTAR DIRECTLY?
// - Calling AStarClass::Instance.AttemptPath with a nullptr unit context and MovementZone::Normal
//   runs the pathfinder abstractly on the map grid. It is stateless and 100% mathematically correct.
bool GeneralUtils::AreZonesConnected(CellStruct fromCell, CellStruct toCell, MovementZone movementZone)
{
	const int returnValue = GetAStarPathLength(fromCell, toCell, movementZone);
	return returnValue > 0 && returnValue < 2147483647;
}

// Computes and returns the list of cells representing the path from fromCell to toCell.
// Returns an empty vector if no path exists.
std::vector<CellStruct> GeneralUtils::GetAStarPath(CellStruct fromCell, CellStruct toCell, MovementZone movementZone)
{
	std::vector<CellStruct> path;

	FootClass* pFoot = GetRepresentativeFootForCell(fromCell);

	CellStruct start = fromCell;
	if (IsCellBlocked(fromCell))
		start = GetPassableNeighbor(fromCell);

	CellStruct end = toCell;
	if (IsCellBlocked(toCell))
		end = GetPassableNeighbor(toCell);

	const int maxSteps = 500;
	int directions[maxSteps] = { 0 };

	PathFinderData* pPathData = AStarClass::Instance.FindPath(
		&start, &end, pFoot, directions, maxSteps, movementZone, 0
	);

	if (pPathData != nullptr && pPathData->PathLength > 0)
	{
		CellStruct currentCell = fromCell;
		path.push_back(currentCell);

		// Direction offsets mapping for 8 directions in engine:
		// 0 = Northeast (1, -1)
		// 1 = East (1, 0)
		// 2 = Southeast (1, 1)
		// 3 = South (0, 1)
		// 4 = Southwest (-1, 1)
		// 5 = West (-1, 0)
		// 6 = Northwest (-1, -1)
		// 7 = North (0, -1)
		const int dx[8] = { 1, 1, 1, 0, -1, -1, -1, 0 };
		const int dy[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };

		// Reconstruct the path cell-by-cell using direction steps
		for (int i = 0; i < pPathData->PathLength; i++)
		{
			int dir = directions[i];
			if (dir >= 0 && dir < 8)
			{
				currentCell.X += dx[dir];
				currentCell.Y += dy[dir];
				path.push_back(currentCell);
			}
		}
	}

	return path;
}
