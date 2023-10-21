#include "Granite/application/application_glue.hpp"

namespace Granite
{
bool query_application_interface(ApplicationQuery query, void *data, size_t size)
{
	if (query == ApplicationQuery::DefaultManagerFlags && size == sizeof(ApplicationQueryDefaultManagerFlags))
	{
		auto *flags = static_cast<ApplicationQueryDefaultManagerFlags *>(data);
		flags->manager_feature_flags = 0;
		return true;
	}
	else
		return false;
}
}