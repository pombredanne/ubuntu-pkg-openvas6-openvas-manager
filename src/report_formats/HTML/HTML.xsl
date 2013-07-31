<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet
    version="1.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:str="http://exslt.org/strings"
    xmlns:date="http://exslt.org/dates-and-times"
    extension-element-prefixes="str date">
  <xsl:output method="html"
              doctype-system="http://www.w3.org/TR/html4/strict.dtd"
              doctype-public="-//W3C//DTD HTML 4.01//EN"
              encoding="UTF-8" />

<!--
OpenVAS Manager
$Id$
Description: Report stylesheet for HTML format.

Authors:
Matthew Mundell <matthew.mundell@greenbone.de>

Copyright:
Copyright (C) 2010 Greenbone Networks GmbH

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

  <!-- <xsl:key name="host_results" match="*/result" use="host" /> -->
  <!-- <xsl:key name="host_ports" match="*/result[port]" use="../host" /> -->

<xsl:template name="prognostic-description">
  <xsl:param name="string"/>

  <xsl:for-each select="str:split($string, '&#10;&#10;')">
    <p>
      <xsl:for-each select="str:split(., '&#10;')">
        <xsl:value-of select="."/>
        <br/>
      </xsl:for-each>
    </p>
  </xsl:for-each>
</xsl:template>

<!-- This is called within a PRE. -->
<xsl:template name="wrap">
  <xsl:param name="string"/>

  <xsl:for-each select="str:tokenize($string, '&#10;')">
    <xsl:call-template name="wrap-line">
      <xsl:with-param name="string" select="."/>
    </xsl:call-template>
    <xsl:text>
</xsl:text>
  </xsl:for-each>
</xsl:template>

<!-- This is called within a PRE. -->
<xsl:template name="wrap-line">
  <xsl:param name="string"/>

  <xsl:variable name="to-next-newline">
    <xsl:value-of select="substring-before($string, '&#10;')"/>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="string-length($string) = 0">
      <!-- The string is empty. -->
    </xsl:when>
    <xsl:when test="(string-length($to-next-newline) = 0) and (substring($string, 1, 1) != '&#10;')">
      <!-- A single line missing a newline, output up to the edge. -->
<xsl:value-of select="substring($string, 1, 90)"/>
      <xsl:if test="string-length($string) &gt; 90">&#8629;
<xsl:call-template name="wrap-line">
  <xsl:with-param name="string" select="substring($string, 91, string-length($string))"/>
</xsl:call-template>
      </xsl:if>
    </xsl:when>
    <xsl:when test="(string-length($to-next-newline) + 1 &lt; string-length($string)) and (string-length($to-next-newline) &lt; 90)">
      <!-- There's a newline before the edge, so output the line. -->
<xsl:value-of select="substring($string, 1, string-length($to-next-newline) + 1)"/>
<xsl:call-template name="wrap-line">
  <xsl:with-param name="string" select="substring($string, string-length($to-next-newline) + 2, string-length($string))"/>
</xsl:call-template>
    </xsl:when>
    <xsl:otherwise>
      <!-- Any newline comes after the edge, so output up to the edge. -->
<xsl:value-of select="substring($string, 1, 90)"/>
      <xsl:if test="string-length($string) &gt; 90">&#8629;
<xsl:call-template name="wrap-line">
  <xsl:with-param name="string" select="substring($string, 91, string-length($string))"/>
</xsl:call-template>
      </xsl:if>
    </xsl:otherwise>
  </xsl:choose>

</xsl:template>

<xsl:template name="highlight-diff">
  <xsl:param name="string"/>

  <xsl:for-each select="str:tokenize($string, '&#10;')">
      <xsl:call-template name="highlight-diff-line">
        <xsl:with-param name="string" select="."/>
      </xsl:call-template>
  </xsl:for-each>
</xsl:template>

<!-- This is called within a PRE. -->
<xsl:template name="highlight-diff-line">
  <xsl:param name="string"/>

  <xsl:variable name="to-next-newline">
    <xsl:value-of select="substring-before($string, '&#10;')"/>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="string-length($string) = 0">
      <!-- The string is empty. -->
    </xsl:when>
    <xsl:when test="(string-length($to-next-newline) = 0) and (substring($string, 1, 1) != '&#10;')">
      <!-- A single line missing a newline, output up to the edge. -->
      <xsl:choose>
        <xsl:when test="(substring($string, 1, 1) = '@')">
<div style="white-space: pre; font-family: monospace; color: #9932CC;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:when test="(substring($string, 1, 1) = '+')">
<div style="white-space: pre; font-family: monospace; color: #006400;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:when test="(substring($string, 1, 1) = '-')">
<div style="white-space: pre; font-family: monospace; color: #B22222;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:otherwise>
<div style="white-space: pre; font-family: monospace;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:otherwise>
      </xsl:choose>
      <xsl:if test="string-length($string) &gt; 90">&#8629;
<xsl:call-template name="highlight-diff-line">
  <xsl:with-param name="string" select="substring($string, 91, string-length($string))"/>
</xsl:call-template>
      </xsl:if>
    </xsl:when>
    <xsl:when test="(string-length($to-next-newline) + 1 &lt; string-length($string)) and (string-length($to-next-newline) &lt; 90)">
      <!-- There's a newline before the edge, so output the line. -->
      <xsl:choose>
        <xsl:when test="(substring($string, 1, 1) = '@')">
<div style="white-space: pre; font-family: monospace; color: #9932CC;">
<xsl:value-of select="substring($string, 1, string-length($to-next-newline) + 1)"/>
</div>
        </xsl:when>
        <xsl:when test="(substring($string, 1, 1) = '+')">
<div style="white-space: pre; font-family: monospace; color: #006400;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:when test="(substring($string, 1, 1) = '-')">
<div style="white-space: pre; font-family: monospace; color: #B22222;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:otherwise>
<div style="white-space: pre; font-family: monospace;">
<xsl:value-of select="substring($string, 1, string-length($to-next-newline) + 1)"/>
</div>
        </xsl:otherwise>
      </xsl:choose>
<xsl:call-template name="highlight-diff-line">
  <xsl:with-param name="string" select="substring($string, string-length($to-next-newline) + 2, string-length($string))"/>
</xsl:call-template>
    </xsl:when>
    <xsl:otherwise>
      <!-- Any newline comes after the edge, so output up to the edge. -->
      <xsl:choose>
        <xsl:when test="(substring($string, 1, 1) = '@')">
<div style="white-space: pre; font-family: monospace;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:when test="(substring($string, 1, 1) = '+')">
<div style="white-space: pre; font-family: monospace; color: #006400;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:when test="(substring($string, 1, 1) = '-')">
<div style="white-space: pre; font-family: monospace; color: #B22222;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:when>
        <xsl:otherwise>
<div style="white-space: pre; font-family: monospace;">
<xsl:value-of select="substring($string, 1, 90)"/>
</div>
        </xsl:otherwise>
      </xsl:choose>
      <xsl:if test="string-length($string) &gt; 90">&#8629;
<xsl:call-template name="hightlight-diff-line">
  <xsl:with-param name="string" select="substring($string, 91, string-length($string))"/>
</xsl:call-template>
      </xsl:if>
    </xsl:otherwise>
  </xsl:choose>

</xsl:template>

  <xsl:template match="scan_start">
    <tr>
      <td>Scan started:</td>
      <td>
        <xsl:if test="string-length (text ())">
          <b><xsl:value-of select="concat (date:day-abbreviation (text ()), ' ', date:month-abbreviation (text ()), ' ', date:day-in-month (text ()), ' ', format-number(date:hour-in-day(text ()), '00'), ':', format-number(date:minute-in-hour(text ()), '00'), ':', format-number(date:second-in-minute(text ()), '00'), ' ', date:year(text ()))"/></b>
        </xsl:if>
      </td>
    </tr>
  </xsl:template>

  <xsl:template match="scan_end">
    <tr>
      <td>Scan ended:</td>
      <td>
        <xsl:if test="string-length (text ())">
          <xsl:value-of select="concat (date:day-abbreviation (text ()), ' ', date:month-abbreviation (text ()), ' ', date:day-in-month (text ()), ' ', format-number(date:hour-in-day(text ()), '00'), ':', format-number(date:minute-in-hour(text ()), '00'), ':', format-number(date:second-in-minute(text ()), '00'), ' ', date:year(text ()))"/>
        </xsl:if>
      </td>
    </tr>
  </xsl:template>

  <xsl:template match="note">
    <xsl:param name="delta">0</xsl:param>
    <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px; background-color: #ffff90;">
      <b>Note</b><xsl:if test="$delta and $delta &gt; 0"> (Result <xsl:value-of select="$delta"/>)</xsl:if><br/>
      <pre>
        <xsl:call-template name="wrap">
          <xsl:with-param name="string" select="text"/>
        </xsl:call-template>
      </pre>
      <xsl:choose>
        <xsl:when test="active='0'">
        </xsl:when>
        <xsl:when test="active='1' and string-length (end_time) &gt; 0">
          Active until: <xsl:value-of select="end_time"/>.<br/>
        </xsl:when>
        <xsl:otherwise>
        </xsl:otherwise>
      </xsl:choose>
      Last modified: <xsl:value-of select="modification_time"/>.
    </div>
  </xsl:template>

  <xsl:template match="override">
    <xsl:param name="delta">0</xsl:param>
    <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px; background-color: #ffff90;">
      <b>
        Override from
        <xsl:choose>
          <xsl:when test="string-length(threat) = 0">
            Any
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select="threat"/>
          </xsl:otherwise>
        </xsl:choose>
        to <xsl:value-of select="new_threat"/></b><xsl:if test="$delta and $delta &gt; 0"> (Result <xsl:value-of select="$delta"/>)</xsl:if><br/>
      <pre>
        <xsl:call-template name="wrap">
          <xsl:with-param name="string" select="text"/>
        </xsl:call-template>
      </pre>
      <xsl:choose>
        <xsl:when test="active='0'">
        </xsl:when>
        <xsl:when test="active='1' and string-length (end_time) &gt; 0">
          Active until: <xsl:value-of select="end_time"/>.<br/>
        </xsl:when>
        <xsl:otherwise>
        </xsl:otherwise>
      </xsl:choose>
      Last modified: <xsl:value-of select="modification_time"/>.
    </div>
  </xsl:template>

  <xsl:template match="nvt">
    <xsl:variable name="cve_ref">
      <xsl:if test="cve != '' and cve != 'NOCVE'">
        <xsl:value-of select="cve/text()"/>
      </xsl:if>
    </xsl:variable>
    <xsl:variable name="bid_ref">
      <xsl:if test="bid != '' and bid != 'NOBID'">
        <xsl:value-of select="bid/text()"/>
      </xsl:if>
    </xsl:variable>
    <xsl:variable name="xref_ref">
      <xsl:if test="xref != '' and xref != 'NOXREF'">
        <xsl:value-of select="xref/text()"/>
      </xsl:if>
    </xsl:variable>

    <xsl:if test="$cve_ref != '' or $bid_ref != '' or $xref_ref != ''">
      <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px;">
        <b>References</b><br/>

        <table>
          <xsl:if test="$cve_ref != ''">
            <tr valign="top">
              <td>CVE:</td>
              <td><xsl:value-of select="$cve_ref"/></td>
            </tr>
          </xsl:if>
          <xsl:if test="$bid_ref != ''">
            <tr valign="top">
              <td>BID:</td>
              <td><xsl:value-of select="$bid_ref"/></td>
            </tr>
          </xsl:if>
          <xsl:if test="$xref_ref != ''">
            <tr valign="top">
              <td>Other:</td>
            </tr>
            <xsl:for-each select="str:split($xref_ref, ',')">
              <tr valign="top">
                <td></td>
                <td><xsl:value-of select="."/></td>
              </tr>
            </xsl:for-each>
          </xsl:if>
        </table>
      </div>
    </xsl:if>
  </xsl:template>

  <xsl:template match="result" mode="issue">
    <xsl:param name="report" select="/report"/>

    <xsl:variable name="style">
      <xsl:choose>
         <xsl:when test="threat='Low'">background:#539dcb</xsl:when>
         <xsl:when test="threat='Medium'">background:#f99f31</xsl:when>
         <xsl:when test="threat='High'">background:#cb1d17</xsl:when>
         <xsl:otherwise>background:#d5d5d5</xsl:otherwise>
      </xsl:choose>
    </xsl:variable>

    <xsl:choose>
      <xsl:when test="$report/@type = 'prognostic'">
        <div style="{$style}; padding:4px; margin:3px; margin-bottom:0px; color: #FFFFFF; border: 1px solid #CCCCCC; border-bottom: 0px;">
          <div style="float: right; text-align:right">
            <xsl:value-of select="cve/cpe/@id"/>
          </div>
          <b><xsl:value-of select="threat"/></b>
          <xsl:if test="string-length(cve/cvss_base) &gt; 0">
             (CVSS: <xsl:value-of select="cve/cvss_base"/>)
          </xsl:if>
          <div><xsl:value-of select="cve/@id"/></div>
        </div>
        <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px;">
          <xsl:call-template name="prognostic-description">
            <xsl:with-param name="string" select="description"/>
          </xsl:call-template>
        </div>
      </xsl:when>
      <xsl:otherwise>
        <div style="{$style}; padding:4px; margin:3px; margin-bottom:0px; color: #FFFFFF; border: 1px solid #CCCCCC; border-bottom: 0px;">
          <div style="float:right; text-align:right">
            <xsl:value-of select="port"/>
          </div>
          <xsl:if test="delta/text()">
            <div style="float: left; font-size: 24px; border: 2px; padding-left: 2px; padding-right: 8px; margin:0px;">
              <xsl:choose>
                <xsl:when test="delta/text() = 'changed'">~</xsl:when>
                <xsl:when test="delta/text() = 'gone'">&#8722;</xsl:when>
                <xsl:when test="delta/text() = 'new'">+</xsl:when>
                <xsl:when test="delta/text() = 'same'">=</xsl:when>
              </xsl:choose>
            </div>
          </xsl:if>
          <b><xsl:value-of select="threat"/></b>
          <xsl:choose>
            <xsl:when test="original_threat">
              <xsl:choose>
                <xsl:when test="threat = original_threat">
                  <xsl:if test="string-length(nvt/cvss_base) &gt; 0">
                     (CVSS: <xsl:value-of select="nvt/cvss_base"/>)
                  </xsl:if>
                </xsl:when>
                <xsl:otherwise>
                  (Overridden from <b><xsl:value-of select="original_threat"/></b>)
                </xsl:otherwise>
              </xsl:choose>
            </xsl:when>
            <xsl:otherwise>
              <xsl:if test="string-length(nvt/cvss_base) &gt; 0">
                 (CVSS: <xsl:value-of select="nvt/cvss_base"/>)
              </xsl:if>
            </xsl:otherwise>
          </xsl:choose>
          <div style="width: 100%">
            NVT:
            <xsl:variable name="max" select="80"/>
              <xsl:choose>
                <xsl:when test="string-length(nvt/name) &gt; $max">
                  <xsl:value-of select="substring(nvt/name, 0, $max)"/>...
                </xsl:when>
                <xsl:otherwise>
                  <xsl:value-of select="nvt/name"/>
                </xsl:otherwise>
              </xsl:choose>
            (OID: <xsl:value-of select="nvt/@oid"/>)
          </div>
        </div>
        <xsl:if test="count (detection)">
          <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px;">
            Product detection result:
            <xsl:value-of select="detection/result/details/detail[name = 'product']/value/text()"/>
            by
            <xsl:value-of select="detection/result/details/detail[name = 'source_name']/value/text()"/>
            (OID: <xsl:value-of select="detection/result/details/detail[name = 'source_oid']/value/text()"/>)
          </div>
        </xsl:if>
        <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px;">
          <xsl:choose>
            <xsl:when test="delta/text() = 'changed'">
              <b>Result 1</b>
            </xsl:when>
          </xsl:choose>
          <pre>
            <xsl:call-template name="wrap">
              <xsl:with-param name="string" select="description"/>
            </xsl:call-template>
          </pre>
        </div>
        <xsl:if test="delta">
          <xsl:choose>
            <xsl:when test="delta/text() = 'changed'">
              <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px;">
                <b>Result 2</b>
                <pre>
                  <xsl:call-template name="wrap">
                    <xsl:with-param name="string" select="delta/result/description"/>
                  </xsl:call-template>
                </pre>
              </div>
              <div style="padding:4px; margin:3px; margin-bottom:0px; margin-top:0px; border: 1px solid #CCCCCC; border-top: 0px;">
                <b>Different Lines</b>
                <p>
                  <xsl:call-template name="highlight-diff">
                    <xsl:with-param name="string" select="delta/diff"/>
                  </xsl:call-template>
                </p>
              </div>
            </xsl:when>
          </xsl:choose>
        </xsl:if>
        <xsl:variable name="delta">
          <xsl:choose>
            <xsl:when test="delta">1</xsl:when>
            <xsl:otherwise>0</xsl:otherwise>
          </xsl:choose>
        </xsl:variable>
        <xsl:apply-templates select="nvt"/>
        <xsl:apply-templates select="notes/note">
          <xsl:with-param name="delta" select="$delta"/>
        </xsl:apply-templates>
        <xsl:apply-templates select="delta/notes/note">
          <xsl:with-param name="delta" select="2"/>
        </xsl:apply-templates>
        <xsl:if test="$report/filters/apply_overrides/text()='1'">
          <xsl:apply-templates select="overrides/override">
            <xsl:with-param name="delta" select="$delta"/>
          </xsl:apply-templates>
          <xsl:apply-templates select="delta/overrides/override">
            <xsl:with-param name="delta" select="2"/>
          </xsl:apply-templates>
        </xsl:if>
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <xsl:template match="report">
    <xsl:choose>
      <xsl:when test="@extension='xml'">
        <xsl:apply-templates select="report"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="real-report"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name="real-report">
    <xsl:choose>
      <xsl:when test="delta">
        <h1>Delta Report Summary</h1>

        <p>
          This document compares the results of two security scans.
          The report first summarises the hosts found.  Then, for each host,
          the report describes the changes that occurred between the two
          scans.
        </p>
      </xsl:when>
      <xsl:when test="@type = 'prognostic'">
        <h1>Prognostic Report Summary</h1>

        <p>
          This document predicts the results of a security scan, based on
          scan information already gathered for the hosts.
          The report first summarises the results found.  Then, for each host,
          the report describes every issue found.  Please consider the
          advice given in each description, in order to rectify the issue.
        </p>
      </xsl:when>
      <xsl:otherwise>
        <h1>Summary</h1>

        <p>
          This document reports on the results of an automatic security scan.
          The report first summarises the results found.  Then, for each host,
          the report describes every issue found.  Please consider the
          advice given in each description, in order to rectify the issue.
        </p>
      </xsl:otherwise>
    </xsl:choose>

    <xsl:choose>
      <xsl:when test="@type = 'prognostic'">
      </xsl:when>
      <xsl:otherwise>
        <p>
          <xsl:choose>
            <xsl:when test="filters/autofp/text()='1'">
              Vendor security updates are trusted, using full CVE matching.
            </xsl:when>
            <xsl:when test="filters/autofp/text()='2'">
              Vendor security updates are trusted, using partial CVE matching.
            </xsl:when>
            <xsl:otherwise>
              Vendor security updates are not trusted.
            </xsl:otherwise>
          </xsl:choose>
        </p>

        <p>
          <xsl:choose>
            <xsl:when test="filters/apply_overrides/text()='1'">
              Overrides are on.  When a result has an override, this report uses the threat of the override.
            </xsl:when>
            <xsl:otherwise>
              Overrides are off.  Even when a result has an override, this report uses the actual threat of the result.
            </xsl:otherwise>
          </xsl:choose>
        </p>

        <p>
          <xsl:choose>
            <xsl:when test="filters/notes = 0">
              Notes are excluded from the report.
            </xsl:when>
            <xsl:otherwise>
              Notes are included in the report.
            </xsl:otherwise>
          </xsl:choose>
        </p>
      </xsl:otherwise>
    </xsl:choose>

    <p>
      This report might not show details of all issues that were found.
      <xsl:if test="filters/result_hosts_only = 1">
        It only lists hosts that produced issues.
      </xsl:if>
      <xsl:if test="string-length(filters/phrase) &gt; 0">
        It shows issues that contain the search phrase "<xsl:value-of select="filters/phrase"/>".
      </xsl:if>
      <xsl:if test="contains(filters/text(), 'h') = false">
        Issues with the threat level "High" are not shown.
      </xsl:if>
      <xsl:if test="contains(filters/text(), 'm') = false">
        Issues with the threat level "Medium" are not shown.
      </xsl:if>
      <xsl:if test="contains(filters/text(), 'l') = false">
        Issues with the threat level "Low" are not shown.
      </xsl:if>
      <xsl:if test="contains(filters/text(), 'g') = false">
        Issues with the threat level "Log" are not shown.
      </xsl:if>
      <xsl:if test="contains(filters/text(), 'd') = false">
        Issues with the threat level "Debug" are not shown.
      </xsl:if>
      <xsl:if test="contains(filters/text(), 'f') = false">
        Issues with the threat level "False Positive" are not shown.
      </xsl:if>
    </p>

    <p>
      <xsl:variable name="last" select="results/@start + count(results/result) - 1"/>
      <xsl:choose>
        <xsl:when test="$last = 0">
          <xsl:text>This report contains 0 results.</xsl:text>
        </xsl:when>
        <xsl:when test="$last = results/@start">
          <xsl:text>This report contains result </xsl:text>
          <xsl:value-of select="$last"/>
          <xsl:text> of the </xsl:text>
          <xsl:value-of select="result_count/filtered"/>
          <xsl:text> results selected by the</xsl:text>
          <xsl:text> filtering above.</xsl:text>
        </xsl:when>
        <xsl:when test="$last = result_count/filtered">
          <xsl:text>This report contains all </xsl:text>
          <xsl:value-of select="result_count/filtered"/>
          <xsl:text> results selected by the</xsl:text>
          <xsl:text> filtering described above.</xsl:text>
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>This report contains results </xsl:text>
          <xsl:value-of select="results/@start"/>
          <xsl:text> to </xsl:text>
          <xsl:value-of select="$last"/>
          <xsl:text> of the </xsl:text>
          <xsl:value-of select="result_count/filtered"/>
          <xsl:text> results selected by the</xsl:text>
          <xsl:text> filtering described above.</xsl:text>
        </xsl:otherwise>
      </xsl:choose>
      <xsl:choose>
        <xsl:when test="@type = 'prognostic'">
        </xsl:when>
        <xsl:when test="delta">
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>  Before filtering there were </xsl:text>
          <xsl:value-of select="result_count/text()"/>
          <xsl:text> results.</xsl:text>
        </xsl:otherwise>
      </xsl:choose>
    </p>

    <xsl:choose>
      <xsl:when test="delta">
        <table>
          <tr>
            <td>Scan 1 started:</td>
            <td><xsl:value-of select="scan_start"/></td>
          </tr>
          <tr>
            <td>Scan 2 started:</td>
            <td><xsl:value-of select="delta/report/scan_start"/></td>
          </tr>
        </table>
        <table>
          <tr>
            <td>Scan 1 ended:</td>
            <td><xsl:value-of select="scan_end"/></td>
          </tr>
          <tr>
            <td>Scan 2 ended:</td>
            <td><xsl:value-of select="delta/report/scan_end"/></td>
          </tr>
        </table>
      </xsl:when>
      <xsl:otherwise>
        <table>
          <xsl:apply-templates select="scan_start" />
          <xsl:apply-templates select="scan_end" />
        </table>
      </xsl:otherwise>
    </xsl:choose>

    <h2>Host Summary</h2>

    <table>
      <tr style="background-color: #d5d5d5;">
        <td>Host</td>
        <td>Start</td>
        <td>End</td>
        <td>High</td>
        <td>Medium</td>
        <td>Low</td>
        <td>Log</td>
        <td>False Positive</td>
      </tr>
      <xsl:for-each select="host" >
        <xsl:variable name="current_host" select="ip" />
        <tr>
          <td>
            <xsl:variable name="hostname" select="detail[name/text() = 'hostname']/value"/>
            <xsl:choose>
              <xsl:when test="$hostname">
                <a href="#{$current_host}"><xsl:value-of select="concat($current_host, ' (', $hostname, ')')"/></a>
              </xsl:when>
              <xsl:otherwise>
                <a href="#{$current_host}"><xsl:value-of select="$current_host"/></a>
              </xsl:otherwise>
            </xsl:choose>
          </td>
          <td>
            <xsl:value-of select="concat (date:month-abbreviation(start/text()), ' ', date:day-in-month(start/text()), ', ', format-number(date:hour-in-day(start/text()), '00'), ':', format-number(date:minute-in-hour(start/text()), '00'), ':', format-number(date:second-in-minute(start/text()), '00'))"/>
          </td>
          <td>
            <xsl:choose>
              <xsl:when test="end/text() != ''">
                <xsl:value-of select="concat (date:month-abbreviation(end/text()), ' ', date:day-in-month(end/text()), ', ', format-number(date:hour-in-day(end/text()), '00'), ':', format-number(date:minute-in-hour(end/text()), '00'), ':', format-number(date:second-in-minute(end/text()), '00'))"/>
              </xsl:when>
              <xsl:otherwise>(not finished)</xsl:otherwise>
            </xsl:choose>
          </td>
          <td><xsl:value-of select="count(../results/result[host/text() = $current_host][threat/text() = 'High'])"/></td>
          <td><xsl:value-of select="count(../results/result[host/text() = $current_host][threat/text() = 'Medium'])"/></td>
          <td><xsl:value-of select="count(../results/result[host/text() = $current_host][threat/text() = 'Low'])"/></td>
          <td><xsl:value-of select="count(../results/result[host/text() = $current_host][threat/text() = 'Log'])"/></td>
          <td><xsl:value-of select="count(../results/result[host/text() = $current_host][threat/text() = 'False Positive'])"/></td>
        </tr>
      </xsl:for-each>
      <tr>
        <td>Total: <xsl:value-of select="count(host_start)"/></td>
        <td></td>
        <td></td>
        <td><xsl:value-of select="count(results/result[threat/text() = 'High'])"/></td>
        <td><xsl:value-of select="count(results/result[threat/text() = 'Medium'])"/></td>
        <td><xsl:value-of select="count(results/result[threat/text() = 'Low'])"/></td>
        <td><xsl:value-of select="count(results/result[threat/text() = 'Log'])"/></td>
        <td><xsl:value-of select="count(results/result[threat/text() = 'False Positive'])"/></td>
      </tr>
    </table>

    <h1>Results per Host</h1>

    <xsl:variable name="report" select="." />
    <xsl:for-each select="host" >
      <xsl:variable name="current_host" select="ip" />

      <h2 id="{$current_host}">Host <xsl:value-of select="$current_host"/></h2>
      <table>
        <xsl:choose>
          <xsl:when test="$report/@type = 'prognostic'">
          </xsl:when>
          <xsl:otherwise>
            <tr>
              <td>Scanning of this host started at:</td>
              <td><xsl:value-of select="start"/></td>
            </tr>
          </xsl:otherwise>
        </xsl:choose>
        <tr>
          <td>Number of results:</td>
          <td>
            <xsl:value-of select="count(../results/result[host/text()=$current_host])"/>
          </td>
        </tr>
      <!-- Number of results: <xsl:value-of select="count(key('host_results', $current_host))"/> -->
      </table>

      <xsl:variable name="cves" select="str:split(detail[name = 'Closed CVEs']/value, ',')"/>
      <xsl:choose>
        <xsl:when test="$report/@type = 'delta'">
        </xsl:when>
        <xsl:when test="$report/filters/show_closed_cves = 1">
          <h2>
            CVEs closed by vendor security updates for <xsl:value-of select="$current_host"/>
          </h2>
          <table class="gbntable" cellspacing="2" cellpadding="4">
            <tr style="background-color: #d5d5d5;">
              <td>CVE</td>
              <td>NVT</td>
            </tr>
            <xsl:variable name="host" select="."/>
            <xsl:for-each select="$cves">
              <tr>
                <td>
                  <xsl:variable name="token" select="/envelope/token"/>
                  <xsl:value-of select="."/>
                </td>
                <td>
                  <xsl:variable name="cve" select="normalize-space(.)"/>
                  <xsl:variable name="closed_cve"
                                select="$host/detail[name = 'Closed CVE' and contains(value, $cve)]"/>
                  <xsl:value-of select="$closed_cve/source/description"/>
                </td>
              </tr>
            </xsl:for-each>
          </table>
        </xsl:when>
      </xsl:choose>

      <xsl:choose>
        <xsl:when test="$report/@type = 'prognostic'">
        </xsl:when>
        <xsl:otherwise>
          <h3>Port Summary for Host <xsl:value-of select="$current_host" /></h3>

          <table>
            <tr style="background-color: #d5d5d5;">
              <td>Service (Port)</td>
              <td>Threat Level</td>
            </tr>

            <xsl:for-each select="../ports/port[host=$current_host]">
              <tr>
                <td><xsl:value-of select="text()"/></td>
                <td><xsl:value-of select="threat"/></td>
              </tr>
            </xsl:for-each>

          <!-- <xsl:apply-templates select="key('host_results', $current_host)" mode="FIX"/> -->

          </table>
        </xsl:otherwise>
      </xsl:choose>

      <h3>Security Issues for Host <xsl:value-of select="$current_host" /></h3>

      <xsl:apply-templates select="../results/result[host/text()=$current_host]" mode="issue">
        <xsl:with-param name="report" select="$report"/>
      </xsl:apply-templates>

    </xsl:for-each>

  </xsl:template>

  <xsl:template match="/">
    <html>
      <head>
        <link rel="stylesheet" type="text/css" href="./style.css" />
        <title>Scan Report</title>
      </head>
      <body style="background-color: #FFFFFF; margin: 0px; font: small Verdana, sans-serif; font-size: 12px; color: #1A1A1A;">
        <div style="width: 98%; width:700px; align: center; margin-left: auto; margin-right: auto;">
          <table style="width: 100%;" cellpadding="3" cellspacing="0">
            <tr>
              <td valign="top">
                <xsl:apply-templates/>
                <div style="text-align: center;">
                  This file was automatically generated.
                </div>
              </td>
            </tr>
          </table>
        </div>
      </body>
    </html>
  </xsl:template>

</xsl:stylesheet>
