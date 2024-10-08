/*
 * TDHCPD - A Dynamic Host Configuration Protocol (DHCP) server
 * Copyright (C) 2024  Tom-Andre Barstad.
 * This software is licensed under the Software Attribution License.
 * See LICENSE for more information.
*/

#include "Network.h"

#include <algorithm>
#include <stdexcept>
#include <ranges>

void Network::configure(NetworkConfiguration&& config, const std::vector<Lease>& leases)
{
    m_networkSpace = config.networkSpace;
    m_networkSize = config.networkSize;
    m_routers = config.routers;
    m_dhcpServerIdentifier = config.dhcpServerIdentifier;
    m_dhcpFirst = config.dhcpFirst;
    m_dhcpLast = config.dhcpLast;
    m_dnsServers = std::move(config.dnsServers);
    m_leaseTime = config.leaseTime;
    m_renewalTime = config.renewalTime;
    m_rebindingTime = config.rebindingTime;
    m_leaseFile = config.leaseFile;

    m_reservationByHw = std::move(config.reservations);

    m_leasesByHw.clear();
    m_leasesByIp.clear();

    for (const auto& lease : leases)
    {
        m_leasesByHw[lease.hwAddress] = lease;
        m_leasesByIp[lease.ipAddress] = lease;
    }

    for (const auto& [hwAddress, ipAddress] : m_reservationByHw)
    {
        m_reservationByIp[ipAddress] = hwAddress;
    }
}

void Network::setNetworkSpace(std::uint32_t networkSpace)
{
    m_networkSpace = networkSpace;
}

std::uint32_t Network::getNetworkSpace() const
{
    return m_networkSpace;
}

void Network::setNetworkSize(std::uint8_t networkSize)
{
    m_networkSize = networkSize;
}

std::uint8_t Network::getNetworkSize() const
{
    return m_networkSize;
}

void Network::setRouterAddress(std::uint32_t routerAddress)
{
    m_routers = routerAddress;
}

std::uint32_t Network::getRouterAddress() const
{
    return m_routers;
}

void Network::setDhcpServerIdentifier(std::uint32_t identifier)
{
    m_dhcpServerIdentifier = identifier;
}

std::uint32_t Network::getDhcpServerIdentifier() const
{
    return m_dhcpServerIdentifier;
}

void Network::setDnsServers(std::vector<std::uint32_t> servers)
{
    m_dnsServers = std::move(servers);
}

const std::vector<std::uint32_t>& Network::getDnsServers() const
{
    return m_dnsServers;
}

void Network::setDhcpRange(std::uint32_t first, std::uint32_t last)
{
    m_dhcpFirst = first;
    m_dhcpLast = last;
}

void Network::setLeaseDuration(std::uint32_t leaseTimeSeconds)
{
    m_leaseTime = leaseTimeSeconds;
}

std::uint32_t Network::getBroadcastAddress() const
{
    return getNetworkSpace() | ~(~0 << (32 - getNetworkSize()));
}

std::uint32_t Network::getLeaseTime() const
{
    return m_leaseTime;
}

std::uint32_t Network::getRenewalTime() const
{
    return m_renewalTime;
}

std::uint32_t Network::getRebindingTime() const
{
    return m_rebindingTime;
}

const std::string& Network::getLeaseFile() const
{
    return m_leaseFile;
}

std::vector<Lease> Network::getAllLeases() const
{
    std::vector<Lease> leases;
    for (const auto& [hwaddr, lease] : m_leasesByHw)
        leases.emplace_back(lease);
    return leases;
}

const Lease& Network::getLease(std::uint64_t hwAddress) const try
{
    return m_leasesByHw.at(hwAddress);
}
catch (const std::out_of_range&)
{
    return m_invalidLease;
}

const Lease& Network::getLease(std::uint32_t ipAddress) const try
{
    return m_leasesByIp.at(ipAddress);
}
catch (const std::out_of_range&)
{
    return m_invalidLease;
}

std::uint32_t Network::getAvailableAddress(std::uint64_t hardwareAddress, std::uint32_t preferredIpAddress)
{
    /*
     * External requests with preferredIpAddress outside the DHCP range is not allowed.
    */
    if (preferredIpAddress > 0 &&
        ((preferredIpAddress < m_dhcpFirst) || (m_dhcpLast < preferredIpAddress)))
    {
        preferredIpAddress = 0;
    }

    /*
     * Reserved address from config file has the greatest precedence.
     * These reservations are allowed to go outside the DHCP range.
    */
    const auto preferredFromConfig = m_reservationByHw.contains(hardwareAddress);
    if (preferredFromConfig)
    {
        preferredIpAddress = m_reservationByHw[hardwareAddress];
    }

    /*
     * Preferred IP is specified, check if it's allowed and if there's any expired lease on it.
     * Check if preferred IP is allowed - within the network and not the first and last address.
     * Disallow if IP is reserved by different hardware address.
     * Address outside the DHCP range is allowed.
    */
    if (preferredIpAddress != 0)
    {
        if (!isIpAllowed(preferredIpAddress)
            || (m_reservationByIp.contains(preferredIpAddress) && m_reservationByIp[preferredIpAddress] != hardwareAddress))
        {
            preferredIpAddress = 0;
        }
        else
        {
            const auto& lease = getLease(preferredIpAddress);
            if (isLeaseEntryValid(lease) && isLeaseExpired(lease))
                removeLease(preferredIpAddress);
        }
    }

    /*
     * As long as we've got no reservations from config, check:
     * - If hardwareAddress has an existing expired lease, remove it
     * - If hardwareAddress has an existing not expired lease, provide that over the preference
    */
    if (!preferredFromConfig)
    {
        const auto& lease = getLease(hardwareAddress);
        if (isLeaseEntryValid(lease))
        {
            if (isLeaseExpired(lease))
                removeLease(hardwareAddress);
            else
                return lease.ipAddress;
        }
    }

    /* Check if the preferred IP address is not in use and use that */
    if (preferredIpAddress != 0)
    {
        const auto& lease = getLease(preferredIpAddress);
        if (!isLeaseEntryValid(lease))
            return preferredIpAddress;
    }

    /* Find the first available address in the network */
    for (std::uint32_t ip = m_dhcpFirst; ip <= m_dhcpLast; ++ip)
    {
        const auto& lease = getLease(ip);
        if ((!isLeaseEntryValid(lease) || isLeaseExpired(lease)) && !isIpReservedInConfig(ip))
            return ip;
    }

    /* IP pool is exhausted */
    return 0;
}

bool Network::reserveAddress(std::uint64_t hardwareAddress, std::uint32_t ipAddress)
{
    /* Check if the IP is allowed - within the network and not the first and last address. */
    if (!isIpAllowed(ipAddress))
    {
        return false;
    }

    /* Allow reserved address to correct hardware address */
    if (m_reservationByHw.contains(hardwareAddress) && m_reservationByHw[hardwareAddress] == ipAddress)
    {
        addLease(hardwareAddress, ipAddress);
        return true;
    }

    /* Check if IP has a non-expired lease by different hardware address */
    {
        const auto& lease = getLease(ipAddress);
        if (!isLeaseExpired(lease) && lease.hwAddress != hardwareAddress)
            return false;
    }

    /* Check if hardware address has an existing lease with different IP (if it does: remove lease) */
    {
        const auto& lease = getLease(hardwareAddress);
        if (isLeaseEntryValid(lease) && lease.ipAddress != ipAddress)
            removeLease(hardwareAddress);
    }

    /* Add lease */
    addLease(hardwareAddress, ipAddress);
    return true;
}

void Network::releaseAddress(std::uint32_t ipAddress)
{
    removeLease(ipAddress);
}

bool Network::isLeaseEntryValid(const Lease& lease)
{
    return lease.startTime != 0;
}

bool Network::isLeaseExpired(const Lease& lease) const
{
    if (!isLeaseEntryValid(lease))
        return true;

    return std::time(nullptr) - lease.startTime > m_leaseTime;
}

bool Network::isIpAllowed(std::uint32_t ipAddress) const
{
    const auto mask = ~0 << (32 - m_networkSize);
    return ((ipAddress & mask) == (m_networkSpace & mask)
            && ipAddress != m_networkSpace // first address
            && ipAddress != (m_networkSpace | ~mask)); // last address
}

void Network::addLease(std::uint64_t hwAddress, std::uint32_t ipAddress)
{
    auto& lease = m_leasesByHw[hwAddress];
    lease.startTime = std::time(nullptr);
    lease.hwAddress = hwAddress;
    lease.ipAddress = ipAddress;
    m_leasesByIp[ipAddress] = lease;

    const auto& leaseFile = getLeaseFile();
    if (!leaseFile.empty())
        Configuration::SavePersistentLeases(getAllLeases(), leaseFile);
}

void Network::removeLease(std::uint64_t hwAddress)
{
    auto lease = getLease(hwAddress);
    if (!isLeaseEntryValid(lease))
        return;

    auto ipAddress = lease.ipAddress;

    m_leasesByHw.erase(hwAddress);
    m_leasesByIp.erase(ipAddress);
}

void Network::removeLease(std::uint32_t ipAddress)
{
    auto lease = getLease(ipAddress);
    if (!isLeaseEntryValid(lease))
        return;

    auto hwAddress = lease.hwAddress;

    m_leasesByIp.erase(ipAddress);
    m_leasesByHw.erase(hwAddress);
}

bool Network::isIpReservedInConfig(std::uint32_t ipAddress) const
{
    return std::ranges::any_of(m_reservationByHw,
                               [&ipAddress](const auto& kv) {
                                   return kv.second == ipAddress;
                               });
}
