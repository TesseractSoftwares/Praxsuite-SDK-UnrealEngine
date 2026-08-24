#include "Core/PraxRoutes.h"

namespace Prax
{
	namespace
	{
		std::string TrimTrailingSlashes(const std::string& In)
		{
			size_t End = In.size();
			while (End > 0 && In[End - 1] == '/') { --End; }
			return In.substr(0, End);
		}

		std::string TrimSurroundingSlashes(const std::string& In)
		{
			size_t Begin = 0;
			size_t End = In.size();
			while (Begin < End && In[Begin] == '/') { ++Begin; }
			while (End > Begin && In[End - 1] == '/') { --End; }
			return In.substr(Begin, End - Begin);
		}
	}

	namespace Routes
	{
		std::string Build(const std::string& Host, const std::string& WorkspaceId,
						  const std::string& Route)
		{
			// A host pasted from a browser usually has a trailing slash and a hand-written route
			// usually has a leading one. Left alone that produces a double slash, which the gateway
			// answers with a 404 that looks like a missing workspace.
			std::string Out = TrimTrailingSlashes(Host);
			const std::string Workspace = TrimSurroundingSlashes(WorkspaceId);
			const std::string Path = TrimSurroundingSlashes(Route);

			if (!Workspace.empty())
			{
				Out += "/" + Workspace;
			}
			if (!Path.empty())
			{
				Out += "/" + Path;
			}
			return Out;
		}

		std::string Query(const std::string& Host, const std::string& WorkspaceId)
		{
			return Build(Host, WorkspaceId, "query");
		}

		std::string Schema(const std::string& Host, const std::string& WorkspaceId)
		{
			return Build(Host, WorkspaceId, "schema");
		}

		std::string Files(const std::string& Host, const std::string& WorkspaceId)
		{
			return Build(Host, WorkspaceId, "files");
		}

		std::string Auth(const std::string& Host, const std::string& WorkspaceId,
						 const std::string& Action)
		{
			return Build(Host, WorkspaceId, "auth/" + TrimSurroundingSlashes(Action));
		}

		std::string Endpoint(const std::string& Host, const std::string& WorkspaceId,
							 const std::string& EndpointId)
		{
			return Build(Host, WorkspaceId, "endpoint/" + TrimSurroundingSlashes(EndpointId));
		}
	}
}
