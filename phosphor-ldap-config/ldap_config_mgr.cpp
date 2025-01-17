#include "ldap_config_mgr.hpp"

#include "ldap_config.hpp"
#include "utils.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace phosphor
{
namespace ldap
{

constexpr auto nscdService = "nscd.service";
constexpr auto LDAPscheme = "ldap";
constexpr auto LDAPSscheme = "ldaps";

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;
namespace fs = std::filesystem;
using Argument = xyz::openbmc_project::Common::InvalidArgument;
using NotAllowed = sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
using NotAllowedArgument = xyz::openbmc_project::Common::NotAllowed;

using Line = std::string;
using Key = std::string;
using Val = std::string;
using ConfigInfo = std::map<Key, Val>;

void ConfigMgr::startOrStopService(const std::string& service, bool start)
{
    if (start)
    {
        restartService(service);
    }
    else
    {
        stopService(service);
    }
}

void ConfigMgr::restartService(const std::string& service)
{
    try
    {
        auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                          SYSTEMD_INTERFACE, "RestartUnit");
        method.append(service.c_str(), "replace");
        bus.call_noreply(method);
    }
    catch (const sdbusplus::exception::SdBusError& ex)
    {
        log<level::ERR>("Failed to restart service",
                        entry("SERVICE=%s", service.c_str()),
                        entry("ERR=%s", ex.what()));
        elog<InternalFailure>();
    }
}
void ConfigMgr::stopService(const std::string& service)
{
    try
    {
        auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                          SYSTEMD_INTERFACE, "StopUnit");
        method.append(service.c_str(), "replace");
        bus.call_noreply(method);
    }
    catch (const sdbusplus::exception::SdBusError& ex)
    {
        log<level::ERR>("Failed to stop service",
                        entry("SERVICE=%s", service.c_str()),
                        entry("ERR=%s", ex.what()));
        elog<InternalFailure>();
    }
}

std::string ConfigMgr::createConfig(
    std::string ldapServerURI, std::string ldapBindDN, std::string ldapBaseDN,
    std::string ldapBindDNPassword, CreateIface::SearchScope ldapSearchScope,
    CreateIface::Create::Type ldapType, std::string groupNameAttribute,
    std::string userNameAttribute)
{
    bool secureLDAP = false;

    if (isValidLDAPURI(ldapServerURI, LDAPSscheme))
    {
        secureLDAP = true;
    }
    else if (isValidLDAPURI(ldapServerURI, LDAPscheme))
    {
        secureLDAP = false;
    }
    else
    {
        log<level::ERR>("bad LDAP Server URI",
                        entry("LDAPSERVERURI=%s", ldapServerURI.c_str()));
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("ldapServerURI"),
                              Argument::ARGUMENT_VALUE(ldapServerURI.c_str()));
    }

    if (secureLDAP && !fs::exists(tlsCacertFile.c_str()))
    {
        log<level::ERR>("LDAP server's CA certificate not provided",
                        entry("TLSCACERTFILE=%s", tlsCacertFile.c_str()));
        elog<NoCACertificate>();
    }

    if (ldapBindDN.empty())
    {
        log<level::ERR>("Not a valid LDAP BINDDN",
                        entry("LDAPBINDDN=%s", ldapBindDN.c_str()));
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("LDAPBindDN"),
                              Argument::ARGUMENT_VALUE(ldapBindDN.c_str()));
    }

    if (ldapBaseDN.empty())
    {
        log<level::ERR>("Not a valid LDAP BASEDN",
                        entry("LDAPBASEDN=%s", ldapBaseDN.c_str()));
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("LDAPBaseDN"),
                              Argument::ARGUMENT_VALUE(ldapBaseDN.c_str()));
    }

    // With current implementation we support only two default LDAP server.
    // which will be always there but when the support comes for additional
    // account providers then the create config would be used to create the
    // additional config.

    std::string objPath;

    if (static_cast<ConfigIface::Type>(ldapType) == ConfigIface::Type::OpenLdap)
    {
        openLDAPConfigPtr.reset(nullptr);
        objPath = openLDAPDbusObjectPath;
        openLDAPConfigPtr = std::make_unique<Config>(
            bus, objPath.c_str(), configFilePath.c_str(), tlsCacertFile.c_str(),
            tlsCertFile.c_str(), secureLDAP, ldapServerURI, ldapBindDN,
            ldapBaseDN, std::move(ldapBindDNPassword),
            static_cast<ConfigIface::SearchScope>(ldapSearchScope),
            static_cast<ConfigIface::Type>(ldapType), false, groupNameAttribute,
            userNameAttribute, *this);
    }
    else
    {
        ADConfigPtr.reset(nullptr);
        objPath = ADDbusObjectPath;
        ADConfigPtr = std::make_unique<Config>(
            bus, objPath.c_str(), configFilePath.c_str(), tlsCacertFile.c_str(),
            tlsCertFile.c_str(), secureLDAP, ldapServerURI, ldapBindDN,
            ldapBaseDN, std::move(ldapBindDNPassword),
            static_cast<ConfigIface::SearchScope>(ldapSearchScope),
            static_cast<ConfigIface::Type>(ldapType), false, groupNameAttribute,
            userNameAttribute, *this);
    }
    restartService(nscdService);
    return objPath;
}

void ConfigMgr::createDefaultObjects()
{
    if (!openLDAPConfigPtr)
    {
        openLDAPConfigPtr = std::make_unique<Config>(
            bus, openLDAPDbusObjectPath.c_str(), configFilePath.c_str(),
            tlsCacertFile.c_str(), tlsCertFile.c_str(),
            ConfigIface::Type::OpenLdap, *this);
        openLDAPConfigPtr->emit_object_added();
    }
    if (!ADConfigPtr)
    {
        ADConfigPtr = std::make_unique<Config>(
            bus, ADDbusObjectPath.c_str(), configFilePath.c_str(),
            tlsCacertFile.c_str(), tlsCertFile.c_str(),
            ConfigIface::Type::ActiveDirectory, *this);
        ADConfigPtr->emit_object_added();
    }
}

bool ConfigMgr::enableService(Config& config, bool value)
{
    if (value)
    {
        if (openLDAPConfigPtr && openLDAPConfigPtr->enabled())
        {
            elog<NotAllowed>(NotAllowedArgument::REASON(
                "OpenLDAP service is already active"));
        }
        if (ADConfigPtr && ADConfigPtr->enabled())
        {
            elog<NotAllowed>(NotAllowedArgument::REASON(
                "ActiveDirectory service is already active"));
        }
    }
    return config.enableService(value);
}

void ConfigMgr::restore()
{
    createDefaultObjects();
    // Restore the ldap config and their mappings
    if (ADConfigPtr->deserialize())
    {
        // Restore the role mappings
        ADConfigPtr->restoreRoleMapping();
        ADConfigPtr->emit_object_added();
    }
    if (openLDAPConfigPtr->deserialize())
    {
        // Restore the role mappings
        openLDAPConfigPtr->restoreRoleMapping();
        openLDAPConfigPtr->emit_object_added();
    }
}

} // namespace ldap
} // namespace phosphor
