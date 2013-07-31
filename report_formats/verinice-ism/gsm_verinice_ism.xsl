<?xml version="1.0" encoding="UTF-8"?>
<!--
OpenVAS Manager
$Id$
Description: Report stylesheet for IT-Grundschutz Verinice interface.

This stylesheet extracts the tables of IT-Grundschutz
scans from the given XML scan report using a XSL
transformation with the tool xsltproc.

Parameters:
- htmlfilename: should contain the filename of a html report
- filedate: should contain the reports modification time as seconds since Epoch

Authors:
Michael Wiegand <michael.wiegand@greenbone.net>
Andre Heinecke <aheinecke@intevation.de>

Copyright:
Copyright (C) 2011, 2012 Greenbone Networks GmbH

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2,
or, at your option, any later version as published by the Free
Software Foundation

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:str="http://exslt.org/strings" version="1.0" extension-element-prefixes="str">
  <xsl:param name="htmlfilename"/>
  <xsl:param name="filedate"/>
  <xsl:include href="classification.xsl"/>
  <xsl:output method="xml" encoding="UTF-8"/>

  <xsl:template match="task">
    <xsl:value-of select="@id"/>
  </xsl:template>

  <xsl:key name="scenarios" match="/report/results/result/nvt/@oid" use="." />
  <xsl:key name="vulnerabilities" match="/report/results/result/nvt/@oid" use="." />
  <xsl:key name="controls" match="/report/results/result/notes/note/@id" use="." />

  <xsl:template name="extract_organization">
      <xsl:choose>
          <!-- TODO enter here the real path of the organization tag -->
          <xsl:when test="string-length(report/task/tags/organization) &gt; 0">
              <xsl:value-of select="report/task/tags/ogranization"/>
          </xsl:when>
          <xsl:when test="string-length(report/task/comment) &gt; 0">
              <xsl:value-of select="report/task/comment"/>
          </xsl:when>
          <xsl:otherwise>
              <xsl:value-of select="report/task/name"/>
          </xsl:otherwise>
      </xsl:choose>
  </xsl:template>

  <!-- Generate the contents of the asset description field -->
  <xsl:template name="get-details">
      <xsl:variable name="addr">
          <xsl:value-of select="ip/text()"/>
      </xsl:variable>
      <!-- Operating System as Text -->
      <syncAttribute>
          <name>gsm_os</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='best_os_txt']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='best_os_txt']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>

      <!-- Operating System as CPE -->

      <syncAttribute>
          <name>gsm_os_cpe</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='best_os_cpe']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='best_os_cpe']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!-- Hostname if available otherwise empty -->
      <syncAttribute>
          <name>gsm_hostname</name>
          <value>
              <xsl:value-of name="string" select="/report/host[ip=$addr]/detail[name='hostname']/value/text()"/>
          </value>
      </syncAttribute>
      <!-- Scan started -->
      <syncAttribute>
          <name>gsm_scan_started</name>
          <value>
              <xsl:value-of select="start/text()"/>
          </value>
      </syncAttribute>
      <!-- Scan ended -->
      <syncAttribute>
          <name>gsm_scan_ended</name>
          <value>
              <xsl:value-of select="end/text()"/>
          </value>
      </syncAttribute>
      <!-- Open Ports -->
      <syncAttribute>
          <name>gsm_open_ports</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='ports']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='ports']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!-- cpuinfo -->
      <syncAttribute>
          <name>gsm_cpuinfo</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='cpuinfo']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='cpuinfo']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!-- memory -->
      <syncAttribute>
          <name>gsm_memory</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='meminfo']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='meminfo']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!--TODO mac-address -->
      <syncAttribute>
          <name>gsm_mac_address</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='MAC']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='MAC']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!-- Traceroute information -->
      <syncAttribute>
          <name>gsm_traceroute</name>
          <value>
              <xsl:choose>
                  <xsl:when test="string-length(/report/host[ip=$addr]/detail[name='traceroute']/value) &gt; 0">
                      <xsl:value-of select="/report/host[ip=$addr]/detail[name='traceroute']/value/text()"/>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!-- Installed software -->
      <syncAttribute>
          <name>gsm_installed_apps</name>
          <value>
              <xsl:choose>
                  <xsl:when test="count(/report/host[ip=$addr]/detail[name='App']) &gt; 0">
                      <xsl:for-each select="/report/host[ip=$addr]/detail[name='App']">
                          <xsl:value-of select="value/text()"/>
<xsl:text>
</xsl:text>
                      </xsl:for-each>
                  </xsl:when>
                  <xsl:otherwise>
                      <xsl:text>Not Available</xsl:text>
                  </xsl:otherwise>
              </xsl:choose>
          </value>
      </syncAttribute>
      <!-- Plain and simple ip address -->
      <syncAttribute>
          <name>gsm_ip_address</name>
          <value>
              <xsl:value-of select="$addr"/>
          </value>
      </syncAttribute>
  </xsl:template>

  <xsl:template match="report/host_start">
    <xsl:param name="task_id"/>
    <xsl:variable name="addr">
      <xsl:value-of select="host"/>
    </xsl:variable>
    <children>
      <syncAttribute>
        <name>gsm_ism_asset_abbr</name>
        <value>
            <!-- Empty for now
          <xsl:value-of select="$addr"/> -->
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_asset_hostname</name>
        <value><!-- Warning can be empty -->
          <xsl:choose>
              <xsl:when test="/report/host[ip=$addr]/detail[name='hostname']/value/text()">
                  <xsl:value-of select="/report/host[ip=$addr]/detail[name='hostname']/value/text()"/>
              </xsl:when>
              <xsl:otherwise>
                  <xsl:value-of select="$addr"/>
              </xsl:otherwise>
            </xsl:choose>
        </value>
      </syncAttribute>
      <syncAttribute>
          <name>gsm_ism_asset_tags</name>
          <value>
              <xsl:for-each select="/report/host[ip=$addr]/detail[name='best_os_cpe']">
                  <xsl:call-template name="generate-tags"/>
              </xsl:for-each>
          </value>
      </syncAttribute>
      <syncAttribute>
          <!-- Everything we can scan is pyhsical -->
          <name>gsm_ism_asset_type</name>
          <value>asset_type_phys</value>
      </syncAttribute>
      <xsl:for-each select="/report/host[ip=$addr]">
        <xsl:call-template name="get-details"/>
      </xsl:for-each>
      <syncAttribute>
        <name>gsm_ism_asset_description</name>
        <value></value>
      </syncAttribute>
      <extId><xsl:value-of select="$task_id"/>-<xsl:value-of select="$addr"/></extId>
      <extObjectType>gsm_ism_asset</extObjectType>
    </children>
  </xsl:template>

  <xsl:template name="vulnerability_details">
    <xsl:param name="task_id"/>
    <children>
      <syncAttribute>
        <name>gsm_ism_vulnerability_name</name>
        <value>
          <xsl:value-of select="nvt/name"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_vulnerability_description</name>
        <value>
          <xsl:value-of select="description/text()"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_vulnerability_cve</name>
        <value>
          <xsl:value-of select="nvt/cve"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_vulnerability_level</name>
        <value>
          <xsl:value-of select="threat"/>
        </value>
      </syncAttribute>
      <extId><xsl:value-of select="$task_id"/>-<xsl:value-of select="nvt/@oid"/>-vulnerability</extId>
      <extObjectType>gsm_ism_vulnerability</extObjectType>
    </children>
  </xsl:template>

  <xsl:template name="control_details">
    <xsl:param name="task_id"/>
    <!-- Filter out lines starting with + and create a comma seperated list of them-->
    <xsl:variable name="description">
      <xsl:for-each select="str:split(text, '&#10;')">
        <xsl:if test="substring(.,0,2) != '+'">
          <xsl:value-of select="."/>
          <xsl:text>&#10;</xsl:text>
        </xsl:if>
      </xsl:for-each>
    </xsl:variable>
    <xsl:variable name="tag_list">
      <xsl:for-each select="str:split(text, '&#10;')">
        <xsl:if test="substring(.,0,2) = '+'">
          <xsl:value-of select="substring(.,2)"/>
          <xsl:text>,</xsl:text>
        </xsl:if>
      </xsl:for-each>
    </xsl:variable>
    <!-- Join the filtered list to be nicely cvs formatted
         we don't want poor verince to have to parse too much-->
    <xsl:variable name="joined_list">
      <xsl:for-each select="str:split($tag_list, ',')">
        <xsl:value-of select="."/>
        <xsl:if test="position() != last()">
          <xsl:text>,</xsl:text>
        </xsl:if>
      </xsl:for-each>
    </xsl:variable>
    <children>
      <syncAttribute>
        <name>gsm_ism_control_name</name>
        <value>
          <xsl:value-of select="nvt/name"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_control_description</name>
        <value>
          <xsl:value-of select="$description"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_control_cpe</name>
        <value>
          <xsl:choose>
            <xsl:when test="count(../../detection)">
              <xsl:value-of select="../../detection/result/details/detail[name = 'product']/value/text()"/>
            </xsl:when>
            <xsl:otherwise>Unknown</xsl:otherwise>
          </xsl:choose>
          </value>
        </syncAttribute>
        <xsl:if test="string-length($joined_list)">
          <syncAttribute>
            <name>gsm_ism_control_tag</name>
            <value><xsl:value-of select="$joined_list"/></value>
          </syncAttribute>
        </xsl:if>
      <extId><xsl:value-of select="$task_id"/>-<xsl:value-of select="@id"/>-control</extId>
      <extObjectType>gsm_ism_control</extObjectType>
    </children>
  </xsl:template>

  <!-- Details of a scenario this is called in the context of an NVT
       element -->
  <xsl:template name="scenario_details">
    <xsl:param name="task_id"/>
    <xsl:variable name="cur_oid">
      <!-- Workaround to avoid confusion in select statements -->
      <xsl:value-of select="@oid"/>
    </xsl:variable>
    <children>
      <syncAttribute>
        <name>gsm_ism_scenario_name</name>
        <value>
          <xsl:value-of select="name"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_scenario_cve</name>
        <value>
          <xsl:value-of select="cve"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_scenario_level</name>
        <value>
          <xsl:value-of select="../threat"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_scenario_description</name>
        <value>
          <xsl:value-of select="/report/results/result[nvt/@oid = $cur_oid]/description/text()"/>
        </value>
      </syncAttribute>
      <syncAttribute>
        <name>gsm_ism_scenario_cvss</name>
        <value>
          <xsl:value-of select="cvss_base"/>
        </value>
      </syncAttribute>
      <extId><xsl:value-of select="$task_id"/>-<xsl:value-of select="$cur_oid"/>-scenario</extId>
      <extObjectType>gsm_ism_scenario</extObjectType>
    </children>
  </xsl:template>

  <xsl:template name="create_links">
    <xsl:param name="task_id"/>
    <xsl:variable name="cur_oid">
      <!-- Workaround to avoid confusion in select statements -->
      <xsl:value-of select="@oid"/>
    </xsl:variable>

    <xsl:for-each select="/report/results/result[nvt/@oid = $cur_oid]">
      <syncLink>
        <dependant><xsl:value-of select="$task_id"/>-<xsl:value-of select="$cur_oid"/>-scenario</dependant>
        <dependency><xsl:value-of select="$task_id"/>-<xsl:value-of select="$cur_oid"/>-vulnerability</dependency>
        <relationId>rel_incscen_vulnerability</relationId>
      </syncLink>
      <syncLink>
        <dependant><xsl:value-of select="$task_id"/>-<xsl:value-of select="$cur_oid"/>-scenario</dependant>
        <dependency><xsl:value-of select="$task_id"/>-<xsl:value-of select="host"/></dependency>
        <relationId>rel_incscen_asset</relationId>
      </syncLink>
    </xsl:for-each>
    <xsl:for-each select="/report/results/result/notes/note[nvt/@oid = $cur_oid]">
      <syncLink>
        <dependant><xsl:value-of select="$task_id"/>-<xsl:value-of select="@id"/>-control</dependant>
        <dependency><xsl:value-of select="$task_id"/>-<xsl:value-of select="$cur_oid"/>-scenario</dependency>
        <relationId>rel_control_incscen</relationId>
      </syncLink>
    </xsl:for-each>
  </xsl:template>

  <!-- The root Match -->
  <xsl:template match="/">
    <xsl:variable name="task_id">
      <xsl:call-template name="extract_organization"/>
      <!--<xsl:apply-templates select="report/task"/>-->
    </xsl:variable>
    <xsl:variable name="scan_name">
      <xsl:call-template name="extract_organization"/>
    </xsl:variable>

    <ns3:syncRequest 
      xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
      xmlns="http://www.sernet.de/sync/data"
      xmlns:ns2="http://www.sernet.de/sync/mapping"
      xmlns:ns3="http://www.sernet.de/sync/sync"
      xsi:schemaLocation="http://www.sernet.de/sync/sync sync.xsd         http://www.sernet.de/sync/data data.xsd         http://www.sernet.de/sync/mapping mapping.xsd" 
      sourceId="{$scan_name}">
      <syncData>
        <syncObject>
          <syncAttribute>
            <name>itverbund_name</name>
            <value><xsl:value-of select="$scan_name"/></value>
          </syncAttribute>
          <syncAttribute>
            <name>gsm_tag</name>
            <value>ap-GSM</value>
          </syncAttribute>
          <extId><xsl:value-of select="$scan_name"/></extId>
          <extObjectType>itverbund</extObjectType>
          <children>
            <syncAttribute>
              <name>gsm_ism_assets_group_name</name>
              <value>Assets GSM-Scan</value>
            </syncAttribute>
            <extId><xsl:value-of select="$task_id"/>-ism-assets</extId>
            <extObjectType>gsm_ism_assets</extObjectType>
            <xsl:apply-templates select="report/host_start">
              <xsl:with-param name="task_id">
                <xsl:value-of select="$task_id"/>
              </xsl:with-param>
            </xsl:apply-templates>
          </children>
          <children>
            <syncAttribute>
              <name>gsm_ism_vulnerabilities_group_name</name>
              <value>Vulnerabilities GSM-Scan</value>
            </syncAttribute>
            <extId><xsl:value-of select="$task_id"/>-ism-vulnerabilities</extId>
            <extObjectType>gsm_ism_vulnerabilities</extObjectType>
            <xsl:for-each select="/report/results/result[count(notes/note) &gt; 0]/nvt[generate-id(@oid) = generate-id(key('vulnerabilities', @oid)[1])]/..">
                <xsl:call-template name="vulnerability_details">
                    <xsl:with-param name="task_id">
                        <xsl:value-of select="$task_id"/>
                    </xsl:with-param>
                </xsl:call-template>
            </xsl:for-each>
        </children>
        <children>
            <syncAttribute>
              <name>gsm_ism_controls_group_name</name>
              <value>Controls GSM-Scan</value>
            </syncAttribute>
            <extId><xsl:value-of select="$task_id"/>-ism-controls</extId>
            <extObjectType>gsm_ism_controls</extObjectType>
            <xsl:for-each select="/report/results/result/notes/note[generate-id(@id) = generate-id(key('controls', @id)[1])]">
                <xsl:call-template name="control_details">
                    <xsl:with-param name="task_id">
                        <xsl:value-of select="$task_id"/>
                    </xsl:with-param>
                </xsl:call-template>
            </xsl:for-each>
        </children>
          <children>
            <syncAttribute>
              <name>gsm_ism_scenarios_group_name</name>
              <value>Scenarios GSM-Scan</value>
            </syncAttribute>
            <extId><xsl:value-of select="$task_id"/>-ism-scenario</extId>
            <extObjectType>gsm_ism_scenarios</extObjectType>
            <!-- Only create one scenario per NVT -->
            <xsl:for-each select="/report/results/result[count(notes/note) &gt; 0]/nvt[generate-id(@oid) = generate-id(key('scenarios', @oid)[1])]">
              <xsl:call-template name="scenario_details">
                <xsl:with-param name="task_id">
                  <xsl:value-of select="$task_id"/>
                </xsl:with-param>
              </xsl:call-template>
            </xsl:for-each>
        </children>
        <!--        <file>
            <syncAttribute>
                <name>attachment_file_name</name>
                <value><xsl:value-of select="$filename"/></value>
            </syncAttribute>
            <syncAttribute>
                <name>attachment_name</name>
                <value><xsl:value-of select="$filename"/></value>
            </syncAttribute>
            <syncAttribute>
                <name>attachment_date</name>
                <value><xsl:value-of select="$filedate"/>000</value>
            </syncAttribute>
            <syncAttribute>
                <name>attachment_mime_type</name>
                <value>xml</value>
            </syncAttribute>
            <extId><xsl:value-of select="$filename"/></extId>
            <file>files/<xsl:value-of select="$filename"/></file>
        </file> -->
        <file>
            <syncAttribute>
                <name>attachment_file_name</name>
                <value><xsl:value-of select="$htmlfilename"/></value>
            </syncAttribute>
            <syncAttribute>
                <name>attachment_name</name>
                <value><xsl:value-of select="$htmlfilename"/></value>
            </syncAttribute>
            <syncAttribute>
                <name>attachment_date</name>
                <value><xsl:value-of select="$filedate"/>000</value>
            </syncAttribute>
            <syncAttribute>
                <name>attachment_mime_type</name>
                <value>html</value>
            </syncAttribute>
            <extId><xsl:value-of select="$htmlfilename"/></extId>
            <file>files/<xsl:value-of select="$htmlfilename"/></file>
        </file>

        </syncObject>
        <xsl:for-each select="/report/results/result[count(notes/note) &gt; 0]/nvt[generate-id(@oid) = generate-id(key('scenarios', @oid)[1])]">
          <xsl:call-template name="create_links">
            <xsl:with-param name="task_id">
              <xsl:value-of select="$task_id"/>
            </xsl:with-param>
          </xsl:call-template>
        </xsl:for-each>
    </syncData>

    <ns2:syncMapping>
      <!-- Org Name  / The root entity -->
      <ns2:mapObjectType intId="org" extId="itverbund">
        <ns2:mapAttributeType intId="org_name" extId="itverbund_name"/>
        <ns2:mapAttributeType intId="org_tag" extId="gsm_tag"/>
      </ns2:mapObjectType>

      <!-- Asset / Host -->
      <ns2:mapObjectType intId="asset" extId="gsm_ism_asset">
        <ns2:mapAttributeType intId="asset_abbr" extId="gsm_ism_asset_abbr"/>
        <ns2:mapAttributeType intId="asset_name" extId="gsm_ism_asset_hostname"/>
        <ns2:mapAttributeType intId="asset_type" extId="gsm_ism_asset_type"/>
        <ns2:mapAttributeType intId="gsm_asset_tag" extId="gsm_ism_asset_tags"/>
        <ns2:mapAttributeType intId="gsm_asset_description" extId="gsm_ism_asset_description"/>
        <ns2:mapAttributeType intId="gsm_installed_apps" extId="gsm_installed_apps"/>
        <ns2:mapAttributeType intId="gsm_traceroute" extId="gsm_traceroute"/>
        <ns2:mapAttributeType intId="gsm_memory" extId="gsm_memory"/>
        <ns2:mapAttributeType intId="gsm_cpuinfo" extId="gsm_cpuinfo"/>
        <ns2:mapAttributeType intId="gsm_os" extId="gsm_os"/>
        <ns2:mapAttributeType intId="gsm_os_cpe" extId="gsm_os_cpe"/>
        <ns2:mapAttributeType intId="gsm_open_ports" extId="gsm_open_ports"/>
        <ns2:mapAttributeType intId="gsm_scan_ended" extId="gsm_scan_ended"/>
        <ns2:mapAttributeType intId="gsm_scan_started" extId="gsm_scan_started"/>
        <ns2:mapAttributeType intId="gsm_hostname" extId="gsm_hostname"/>
        <ns2:mapAttributeType intId="gsm_mac_address" extId="gsm_mac_address"/>
        <ns2:mapAttributeType intId="gsm_ip_address" extId="gsm_ip_address"/>
      </ns2:mapObjectType>

      <!-- Vulnerability / NVT -->
      <ns2:mapObjectType intId="vulnerability" extId="gsm_ism_vulnerability">
        <ns2:mapAttributeType intId="vulnerability_name" extId="gsm_ism_vulnerability_name"/>
        <ns2:mapAttributeType intId="gsm_ism_vulnerability_description" extId="gsm_ism_vulnerability_description"/>
        <ns2:mapAttributeType intId="gsm_ism_vulnerability_cve" extId="gsm_ism_vulnerability_cve"/>
        <ns2:mapAttributeType intId="gsm_ism_vulnerability_level" extId="gsm_ism_vulnerability_level"/>
      </ns2:mapObjectType>

      <!-- Control / Note on a vulnerability -->
      <ns2:mapObjectType intId="control" extId="gsm_ism_control">
       <ns2:mapAttributeType intId="control_name" extId="gsm_ism_control_name"/>
       <ns2:mapAttributeType intId="gsm_ism_control_description" extId="gsm_ism_control_description"/>
       <ns2:mapAttributeType intId="gsm_ism_control_cpe" extId="gsm_ism_control_cpe"/>
       <ns2:mapAttributeType intId="gsm_ism_control_tag" extId="gsm_ism_control_tag"/>
      </ns2:mapObjectType>

      <!-- Scenario / NVT -->
      <ns2:mapObjectType intId="incident_scenario" extId="gsm_ism_scenario">
        <ns2:mapAttributeType intId="incident_scenario_name" extId="gsm_ism_scenario_name"/>
        <ns2:mapAttributeType intId="gsm_ism_scenario_description" extId="gsm_ism_scenario_description"/>
        <ns2:mapAttributeType intId="gsm_ism_scenario_cve" extId="gsm_ism_scenario_cve"/>
        <ns2:mapAttributeType intId="gsm_ism_scenario_level" extId="gsm_ism_scenario_level"/>
        <ns2:mapAttributeType intId="gsm_ism_scenario_cvss" extId="gsm_ism_scenario_cvss"/>
      </ns2:mapObjectType>

      <!-- The Rest -->
      <ns2:mapObjectType intId="assetgroup" extId="gsm_ism_assets">
        <ns2:mapAttributeType intId="assetgroup_name" extId="gsm_ism_assets_group_name"/>
      </ns2:mapObjectType>

      <ns2:mapObjectType intId="vulnerability_group" extId="gsm_ism_vulnerabilities">
        <ns2:mapAttributeType intId="vulnerability_group_name" extId="gsm_ism_vulnerabilities_group_name"/>
      </ns2:mapObjectType>

      <ns2:mapObjectType intId="controlgroup" extId="gsm_ism_controls">
        <ns2:mapAttributeType intId="controlgroup_name" extId="gsm_ism_controls_group_name"/>
      </ns2:mapObjectType>

      <ns2:mapObjectType intId="incident_scenario_group" extId="gsm_ism_scenarios">
        <ns2:mapAttributeType intId="incident_scenario_group_name" extId="gsm_ism_scenarios_group_name"/>
      </ns2:mapObjectType>

      <ns2:mapObjectType intId="attachment" extId="attachment">
          <ns2:mapAttributeType intId="attachment_text" extId="attachment_text"/>
          <ns2:mapAttributeType intId="attachment_file_name" extId="attachment_file_name"/>
          <ns2:mapAttributeType intId="attachment_version" extId="attachment_version"/>
          <ns2:mapAttributeType intId="attachment_name" extId="attachment_name"/>
          <ns2:mapAttributeType intId="attachment_date" extId="attachment_date"/>
          <ns2:mapAttributeType intId="attachment_mime_type" extId="attachment_mime_type"/>
          <ns2:mapAttributeType intId="attachment_approval" extId="attachment_approval"/>
          <ns2:mapAttributeType intId="attachment_publish" extId="attachment_publish"/>
      </ns2:mapObjectType>
    </ns2:syncMapping>
  </ns3:syncRequest>
  </xsl:template>
</xsl:stylesheet>
