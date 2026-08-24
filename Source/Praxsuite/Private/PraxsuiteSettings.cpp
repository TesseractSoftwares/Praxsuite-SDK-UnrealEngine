#include "PraxsuiteSettings.h"

#include "Core/PraxRoutes.h"

UPraxsuiteSettings::UPraxsuiteSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Praxsuite");

	// The cloud tier as a starting value, not a default to rely on. A dedicated-tier workspace has
	// its own host, and leaving this at the cloud host is the mistake that produces a project where
	// every call 404s for no visible reason.
	GatewayHost = UTF8_TO_TCHAR(Prax::Routes::CloudHost);
}

const UPraxsuiteSettings* UPraxsuiteSettings::Get()
{
	return GetDefault<UPraxsuiteSettings>();
}
