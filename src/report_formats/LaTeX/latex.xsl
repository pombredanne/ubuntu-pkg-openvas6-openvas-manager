<?xml version="1.0"?>

<!--
Greenbone Security Assistant
$Id$
Description: Stylesheet to transform result (report) xml to latex.

Authors:
Felix Wolfsteller <felix.wolfsteller@greenbone.net>

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

TODOS: Solve Whitespace/Indentation problem of this file.
-->

<xsl:stylesheet
    version="1.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:func = "http://exslt.org/functions"
    xmlns:str="http://exslt.org/strings"
    xmlns:openvas="http://openvas.org"
    xmlns:date="http://exslt.org/dates-and-times"
    extension-element-prefixes="str func date">
  <xsl:output method="text" encoding="string" indent="no"/>
  <xsl:strip-space elements="*"/>

  <func:function name="openvas:report">
    <xsl:choose>
      <xsl:when test="count(/report/report) &gt; 0">
        <func:result select="/report/report"/>
      </xsl:when>
      <xsl:otherwise>
        <func:result select="/report"/>
      </xsl:otherwise>
    </xsl:choose>
  </func:function>

  <xsl:template match="scan_start" name="format-date">
    <xsl:param name="date" select="."/>
    <xsl:if test="string-length ($date)">
      <xsl:choose>
        <xsl:when test="contains($date, '+')">
          <xsl:value-of select="concat (date:day-abbreviation ($date), ' ', date:month-abbreviation ($date), ' ', date:day-in-month ($date), ' ', format-number(date:hour-in-day($date), '00'), ':', format-number(date:minute-in-hour($date), '00'), ':', format-number(date:second-in-minute($date), '00'), ' ', date:year($date), ' ', substring-after ($date, '+'))"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:value-of select="concat (date:day-abbreviation ($date), ' ', date:month-abbreviation ($date), ' ', date:day-in-month ($date), ' ', format-number(date:hour-in-day($date), '00'), ':', format-number(date:minute-in-hour($date), '00'), ':', format-number(date:second-in-minute($date), '00'), ' ', date:year($date), ' UTC')"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:if>
  </xsl:template>

  <xsl:template match="scan_end">
    <xsl:param name="date" select="."/>
    <xsl:if test="string-length ($date)">
      <xsl:choose>
        <xsl:when test="contains($date, '+')">
          <xsl:value-of select="concat (date:day-abbreviation ($date), ' ', date:month-abbreviation ($date), ' ', date:day-in-month ($date), ' ', format-number(date:hour-in-day($date), '00'), ':', format-number(date:minute-in-hour($date), '00'), ':', format-number(date:second-in-minute($date), '00'), ' ', date:year($date), ' ', substring-after ($date, '+'))"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:value-of select="concat (date:day-abbreviation ($date), ' ', date:month-abbreviation ($date), ' ', date:day-in-month ($date), ' ', format-number(date:hour-in-day($date), '00'), ':', format-number(date:minute-in-hour($date), '00'), ':', format-number(date:second-in-minute($date), '00'), ' ', date:year($date), ' UTC')"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:if>
  </xsl:template>

  <!-- A newline, after countless failed tries to define a newline-entity. -->
  <xsl:template name="newline">
    <xsl:text>
</xsl:text>
  </xsl:template>


<!-- TEMPLATES MATCHING LATEX COMMANDS -->

  <!-- Simple Latex Context. -->
  <xsl:template name="latex-simple-command">
    <xsl:param name="command"/>
    <xsl:param name="content"/>
    <xsl:text>\</xsl:text>
    <xsl:value-of select="command"/>
    <xsl:text>{</xsl:text>
    <xsl:value-of select="$content"/>
    <xsl:text>}</xsl:text>
    <xsl:call-template name="newline"/>
  </xsl:template>

  <!-- A Latex Label. -->
  <xsl:template name="latex-label">
    <xsl:param name="label_string"/>
    <xsl:text>\label{</xsl:text><xsl:value-of select="$label_string"/><xsl:text>}</xsl:text>
    <xsl:call-template name="newline"/>
  </xsl:template>

  <!-- A Latex Section. -->
  <xsl:template name="latex-section">
    <xsl:param name="section_string"/>
    <xsl:text>\section{</xsl:text><xsl:value-of select="$section_string"/><xsl:text>}</xsl:text>
    <xsl:call-template name="newline"/>
  </xsl:template>

  <!-- A Latex Subsection. -->
  <xsl:template name="latex-subsection">
    <xsl:param name="subsection_string"/>
    <xsl:text>\subsection{</xsl:text>
    <xsl:call-template name="escape_text">
      <xsl:with-param name="string" select="$subsection_string"/>
    </xsl:call-template>
    <xsl:text>}</xsl:text>
    <xsl:call-template name="newline"/>
  </xsl:template>

  <!-- A Latex Subsubsection. -->
  <xsl:template name="latex-subsubsection">
    <xsl:param name="subsubsection_string"/>
    <xsl:text>\subsubsection{</xsl:text>
    <xsl:call-template name="escape_text">
      <xsl:with-param name="string" select="$subsubsection_string"/>
    </xsl:call-template>
    <xsl:text>}</xsl:text>
    <xsl:call-template name="newline"/>
  </xsl:template>

  <!-- \\ -->
  <xsl:template name="latex-newline">
    <xsl:text>\\</xsl:text><xsl:call-template name="newline"/>
  </xsl:template>

  <!-- Latex \hline command. -->
  <xsl:template name="latex-hline">
    <xsl:text>\hline</xsl:text><xsl:call-template name="newline"/>
  </xsl:template>

  <!-- Latex \hyperref command. -->
  <xsl:template name="latex-hyperref">
    <xsl:param name="target"/>
    <xsl:param name="text"/>
    <xsl:text>\hyperref[</xsl:text>
    <xsl:value-of select="$target"/>
    <xsl:text>]{</xsl:text>
    <xsl:call-template name="escape_text">
      <xsl:with-param name="string" select="$text"/>
    </xsl:call-template>
    <xsl:text>}</xsl:text>
  </xsl:template>

<!-- BUILDING- BLOCK- TEMPLATES -->

  <!-- The longtable block to defined what to print if a page break falls
       within a table. -->
  <xsl:template name="longtable-continue-block">
    <xsl:param name="number-of-columns"/>
    <xsl:param name="header-color"/>
    <xsl:param name="header-text"/>
    <xsl:text>\rowcolor{</xsl:text><xsl:value-of select="$header-color"/><xsl:text>}</xsl:text><xsl:value-of select="$header-text"/>
    <xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\endfirsthead</xsl:text><xsl:call-template name="newline"/>
    <xsl:text>\multicolumn{</xsl:text><xsl:value-of select="$number-of-columns"/><xsl:text>}{l}{\hfill\ldots (continued) \ldots}</xsl:text>
    <xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\rowcolor{</xsl:text><xsl:value-of select="$header-color"/><xsl:text>}</xsl:text><xsl:value-of select="$header-text"/>
    <xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\endhead</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\multicolumn{</xsl:text><xsl:value-of select="$number-of-columns"/><xsl:text>}{l}{\ldots (continues) \ldots}</xsl:text><xsl:call-template name="latex-newline"/>
    <xsl:text>\endfoot</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\endlastfoot</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
  </xsl:template>

  <!-- The latex header. -->
  <xsl:template name="header">
    <xsl:text>\documentclass{article}
\pagestyle{empty}

%\usepackage{color}
\usepackage{tabularx}
\usepackage{geometry}
\usepackage{comment}
\usepackage{longtable}
\usepackage{titlesec}
\usepackage{chngpage}
\usepackage{calc}
\usepackage{url}
\usepackage[utf8x]{inputenc}

\DeclareUnicodeCharacter {135}{{\textascii ?}}
\DeclareUnicodeCharacter {129}{{\textascii ?}}
\DeclareUnicodeCharacter {128}{{\textascii ?}}

\usepackage{colortbl}

% must come last
\usepackage{hyperref}
\definecolor{linkblue}{rgb}{0.11,0.56,1}
\definecolor{inactive}{rgb}{0.56,0.56,0.56}
\definecolor{openvas_debug}{rgb}{0.78,0.78,0.78}
\definecolor{openvas_false_positive}{rgb}{0.2275,0.2275,0.2275}
\definecolor{openvas_log}{rgb}{0.2275,0.2275,0.2275}
\definecolor{openvas_hole}{rgb}{0.7960,0.1137,0.0902}
\definecolor{openvas_note}{rgb}{0.3255,0.6157,0.7961}
\definecolor{openvas_report}{rgb}{0.68,0.74,0.88}
\definecolor{openvas_user_note}{rgb}{1.0,1.0,0.5625}
\definecolor{openvas_user_override}{rgb}{1.0,1.0,0.5625}
\definecolor{openvas_warning}{rgb}{0.9764,0.6235,0.1922}
\definecolor{chunk}{rgb}{0.9412,0.8275,1}
\definecolor{line_new}{rgb}{0.89,1,0.89}
\definecolor{line_gone}{rgb}{1.0,0.89,0.89}
\hypersetup{colorlinks=true,linkcolor=linkblue,urlcolor=blue,bookmarks=true,bookmarksopen=true}
\usepackage[all]{hypcap}

%\geometry{verbose,a4paper,tmargin=24mm,bottom=24mm}
\geometry{verbose,a4paper}
\setlength{\parskip}{\smallskipamount}
\setlength{\parindent}{0pt}
</xsl:text>
<xsl:choose>
  <xsl:when test="openvas:report()/delta">
    <xsl:text>\title{Delta Report}</xsl:text>
  </xsl:when>
  <xsl:when test="openvas:report()/@type = 'prognostic'">
    <xsl:text>\title{Prognostic Report}</xsl:text>
  </xsl:when>
  <xsl:otherwise>
    <xsl:text>\title{Scan Report}</xsl:text>
  </xsl:otherwise>
</xsl:choose>
<xsl:call-template name="latex-newline"/>
<xsl:text>
\pagestyle{headings}
\pagenumbering{arabic}
</xsl:text>
  </xsl:template>

  <!-- Replaces backslash characters by $\backslash$ and $ by \$ -->
  <xsl:template name="latex-replace-backslash-dollar">
    <xsl:param name="string"/>
    <xsl:choose>
      <xsl:when test="contains($string, '\') or contains($string, '$')">
        <xsl:variable name="before_backslash" select="substring-before($string, '\')"/>
        <xsl:variable name="before_dollar" select="substring-before($string, '$')"/>
        <xsl:variable name="strlen_before_backslash" select="string-length($before_backslash)"/>
        <xsl:variable name="strlen_before_dollar" select="string-length($before_dollar)"/>
        <xsl:choose>
          <xsl:when test="$strlen_before_dollar &gt; 0 and $strlen_before_backslash &gt; 0">
            <!-- string contains both $ and \ . -->
            <xsl:choose>
              <xsl:when test="$strlen_before_dollar &gt; $strlen_before_backslash">
                <!-- $ before \. -->
                <xsl:value-of select="concat(substring-before($string, '$'), '\$')"/>
                <xsl:call-template name="latex-replace-backslash-dollar">
                  <xsl:with-param name="string" select="substring-after($string, '$')"/>
                </xsl:call-template>
              </xsl:when>
              <xsl:otherwise>
                <!-- \ before $. -->
                <xsl:value-of select="concat(substring-before($string, '\'), '$\backslash$')"/>
                <xsl:call-template name="latex-replace-backslash-dollar">
                  <xsl:with-param name="string" select="substring-after($string, '\')"/>
                </xsl:call-template>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:when>
          <xsl:when test="string-length($before_backslash) &gt; 0">
            <!-- Only \ is occuring -->
            <xsl:value-of select="concat(substring-before($string, '\'),'$\backslash$')"/>
            <xsl:call-template name="latex-replace-backslash-dollar">
              <xsl:with-param name="string" select="substring-after($string, '\')"/>
            </xsl:call-template>
          </xsl:when>
          <xsl:otherwise>
            <!-- Only $ is occuring -->
            <xsl:value-of select="concat(substring-before($string, '$'),'\$')"/>
            <xsl:call-template name="latex-replace-backslash-dollar">
              <xsl:with-param name="string" select="substring-after($string, '$')"/>
            </xsl:call-template>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:otherwise>
        <!-- Neither $ nor \ occuring. -->
        <xsl:value-of select="$string"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name="escape_special_chars">
    <xsl:param name="string"/>
    <xsl:value-of select="str:replace(
      str:replace(
      str:replace(
      str:replace(
      str:replace(
      str:replace(
      str:replace(
      $string,
      '_', '\_'),
      '%', '\%'),
      '&amp;','\&amp;'),
      '#', '\#'),
      '^', '\^'),
      '}', '\}'),
      '{', '\}')"/>
  </xsl:template>

  <!-- Escape text for normal latex environment. Following characters get a
       prepended backslash: #$%&_^{} -->
  <xsl:template name="escape_text">
    <xsl:param name="string"/>
    <!-- Replace backslashes and $'s .-->
    <xsl:choose>
      <xsl:when test="contains($string, '\') or contains($string, '$')">
        <xsl:call-template name="escape_special_chars">
          <xsl:with-param name="string">
            <xsl:call-template name="latex-replace-backslash-dollar">
              <xsl:with-param name="string" select="$string"/>
            </xsl:call-template>
          </xsl:with-param>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="escape_special_chars">
          <xsl:with-param name="string" select="$string"/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- Create a verbatim indented row. -->
  <xsl:template name="wrap-row-indented">
    <xsl:param name="line"/>
    <xsl:param name="color">white</xsl:param>
    <xsl:text>\rowcolor{</xsl:text>
    <xsl:value-of select="$color"/>
    <xsl:text>}{$\hookrightarrow$\verb=</xsl:text>
    <xsl:value-of select="str:replace($line, '=', '=\verb-=-\verb=')"/>
    <!-- Inline latex-newline for speed. -->
    <xsl:text>=}\\
</xsl:text>
  </xsl:template>

  <!-- Create a verbatim row. -->
  <!-- This is called very often, and is relatively slow. -->
  <xsl:template name="wrap-row">
    <xsl:param name="line"/>
    <xsl:text>\rowcolor{white}{\verb=</xsl:text>
    <xsl:value-of select="str:replace($line, '=', '=\verb-=-\verb=')"/>
    <!-- Inline latex-newline for speed. -->
    <xsl:text>=}\\
</xsl:text>
  </xsl:template>

  <!-- Create a verbatim row. -->
  <!-- This is called very often, and is relatively slow. -->
  <xsl:template name="wrap-row-color">
    <xsl:param name="line"/>
    <xsl:param name="color">white</xsl:param>
    <xsl:text>\rowcolor{</xsl:text>
    <xsl:value-of select="$color"/>
    <xsl:text>}{\verb=</xsl:text>
    <xsl:value-of select="str:replace($line, '=', '=\verb-=-\verb=')"/>
    <!-- Inline latex-newline for speed. -->
    <xsl:text>=}\\
</xsl:text>
  </xsl:template>

  <!-- Takes a string that does not contain a newline char and outputs $max
       characters long lines. -->
  <xsl:template name="break-into-rows-indented">
    <xsl:param name="string"/>
    <xsl:variable name="head" select="substring($string, 1, 78)"/>
    <xsl:variable name="tail" select="substring($string, 79)"/>
    <xsl:if test="string-length($head) &gt; 0">
      <xsl:call-template name="wrap-row-indented">
        <xsl:with-param name="line" select="$head"/>
      </xsl:call-template>
    </xsl:if>
    <xsl:if test="string-length($tail) &gt; 0">
      <xsl:call-template name="break-into-rows-indented">
        <xsl:with-param name="string" select="$tail"/>
      </xsl:call-template>
    </xsl:if>
  </xsl:template>

  <!-- Takes a string that does not contain a newline char and outputs $max
       characters long lines. -->
  <xsl:template name="break-into-rows-indented-color">
    <xsl:param name="string"/>
    <xsl:param name="color">white</xsl:param>
    <xsl:variable name="head" select="substring($string, 1, 78)"/>
    <xsl:variable name="tail" select="substring($string, 79)"/>
    <xsl:if test="string-length($head) &gt; 0">
      <xsl:call-template name="wrap-row-indented">
        <xsl:with-param name="line" select="$head"/>
        <xsl:with-param name="color" select="$color"/>
      </xsl:call-template>
    </xsl:if>
    <xsl:if test="string-length($tail) &gt; 0">
      <xsl:call-template name="break-into-rows-indented-color">
        <xsl:with-param name="string" select="$tail"/>
        <xsl:with-param name="color" select="$color"/>
      </xsl:call-template>
    </xsl:if>
  </xsl:template>

  <!-- Takes a string that does not contain a newline char and outputs $max
       characters long lines. -->
  <xsl:template name="break-into-rows">
    <xsl:param name="string"/>
    <xsl:variable name="head" select="substring($string, 1, 80)"/>
    <xsl:variable name="tail" select="substring($string, 81)"/>
    <xsl:if test="string-length($head) &gt; 0">
      <xsl:call-template name="wrap-row">
        <xsl:with-param name="line" select="$head"/>
      </xsl:call-template>
    </xsl:if>
    <xsl:if test="string-length($tail) &gt; 0">
      <xsl:call-template name="break-into-rows-indented">
        <xsl:with-param name="string" select="$tail"/>
      </xsl:call-template>
    </xsl:if>
  </xsl:template>

  <!-- Takes a string that does not contain a newline char and outputs $max
       characters long lines. -->
  <xsl:template name="break-into-rows-color">
    <xsl:param name="string"/>
    <xsl:param name="color">white</xsl:param>
    <xsl:variable name="head" select="substring($string, 1, 80)"/>
    <xsl:variable name="tail" select="substring($string, 81)"/>
    <xsl:if test="string-length($head) &gt; 0">
      <xsl:call-template name="wrap-row-color">
        <xsl:with-param name="line" select="$head"/>
        <xsl:with-param name="color" select="$color"/>
      </xsl:call-template>
    </xsl:if>
    <xsl:if test="string-length($tail) &gt; 0">
      <xsl:call-template name="break-into-rows-indented-color">
        <xsl:with-param name="string" select="$tail"/>
        <xsl:with-param name="color" select="$color"/>
      </xsl:call-template>
    </xsl:if>
  </xsl:template>

  <!-- -->
  <xsl:template name="text-to-escaped-row">
    <xsl:param name="string"/>
    <xsl:for-each select="str:tokenize($string, '&#10;')">
      <xsl:call-template name="break-into-rows">
        <xsl:with-param name="string" select="."/>
      </xsl:call-template>
    </xsl:for-each>
  </xsl:template>

  <!-- -->
  <xsl:template name="text-to-escaped-row-color">
    <xsl:param name="string"/>
    <xsl:param name="color">white</xsl:param>
    <xsl:for-each select="str:tokenize($string, '&#10;')">
      <xsl:call-template name="break-into-rows-color">
        <xsl:with-param name="string" select="."/>
        <xsl:with-param name="color" select="$color"/>
      </xsl:call-template>
    </xsl:for-each>
  </xsl:template>

  <!-- -->
  <xsl:template name="text-to-escaped-diff-row">
    <xsl:param name="string"/>
    <xsl:for-each select="str:tokenize($string, '&#10;')">
      <xsl:call-template name="break-into-rows-color">
        <xsl:with-param name="string" select="."/>
        <xsl:with-param name="color">
          <xsl:choose>
            <xsl:when test="substring(., 1, 2) = '@@'">chunk</xsl:when>
            <xsl:when test="substring(., 1, 1) = '-'">line_gone</xsl:when>
            <xsl:when test="substring(., 1, 1) = '+'">line_new</xsl:when>
            <xsl:otherwise>white</xsl:otherwise>
          </xsl:choose>
        </xsl:with-param>
      </xsl:call-template>
    </xsl:for-each>
  </xsl:template>

  <!-- The Abstract. -->
  <xsl:template name="abstract">
    <xsl:choose>
      <xsl:when test="openvas:report()/@type = 'prognostic' and openvas:report()/report_format/param[name='summary']">
        <xsl:text>
\renewcommand{\abstractname}{Prognostic Report Summary}
\begin{abstract}
</xsl:text>
        <xsl:value-of select="openvas:report()/report_format/param[name='summary']/value"/>
        <xsl:text>
\end{abstract}
</xsl:text>
      </xsl:when>
      <xsl:when test="openvas:report()/delta and openvas:report()/report_format/param[name='summary']">
        <xsl:text>
\renewcommand{\abstractname}{Delta Report Summary}
\begin{abstract}
</xsl:text>
        <xsl:value-of select="openvas:report()/report_format/param[name='summary']/value"/>
        <xsl:text>
\end{abstract}
</xsl:text>
      </xsl:when>
      <xsl:when test="openvas:report()/report_format/param[name='summary']">
        <xsl:text>
\renewcommand{\abstractname}{Summary}
\begin{abstract}
</xsl:text>
        <xsl:value-of select="openvas:report()/report_format/param[name='summary']/value"/>
        <xsl:text>
\end{abstract}
</xsl:text>
      </xsl:when>
      <xsl:when test="openvas:report()/@type = 'prognostic'">
        <xsl:text>
\renewcommand{\abstractname}{Prognostic Report Summary}
\begin{abstract}
This document predicts the results of a security scan, based on
scan information already gathered for the hosts.
The report first summarises the predicted results.  Then, for each host,
the report describes every predicted issue.  Please consider the
advice given in each description, in order to rectify the issue.
\end{abstract}
</xsl:text>
      </xsl:when>
      <xsl:when test="openvas:report()/delta">
        <xsl:text>
\renewcommand{\abstractname}{Delta Report Summary}
\begin{abstract}
This document compares the results of two security scans.
The first scan started at </xsl:text>
        <xsl:apply-templates select="scan_start"/>
<xsl:text> and ended at </xsl:text>
          <xsl:value-of select="scan_end"/>
<xsl:text>.
The second scan started at </xsl:text>
        <xsl:apply-templates select="delta/report/scan_start"/>
<xsl:text> and ended at </xsl:text>
        <xsl:apply-templates select="delta/report/scan_start"/>
<xsl:text>.
The report first summarises the hosts found.  Then, for each host,
the report describes the changes that occurred between the two scans.
\end{abstract}
</xsl:text>
      </xsl:when>
      <xsl:otherwise>
        <xsl:text>
\renewcommand{\abstractname}{Summary}
\begin{abstract}
This document reports on the results of an automatic security scan.
The scan started at </xsl:text>
        <xsl:apply-templates select="scan_start"/>
<xsl:text> and ended at </xsl:text>
        <xsl:apply-templates select="scan_end"/>
<xsl:text>.  The
report first summarises the results found.  Then, for each host,
the report describes every issue found.  Please consider the
advice given in each description, in order to rectify the issue.
\end{abstract}
</xsl:text>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- The Table of Contents. -->
  <xsl:template name="toc">
    <xsl:text>\tableofcontents</xsl:text><xsl:call-template name="newline"/>
  </xsl:template>

  <!-- Find the highest Threat for a host. -->
  <xsl:template name="highest-severity-for-host">
    <xsl:param name="host"/>
    <xsl:choose>
      <xsl:when test="openvas:report()/ports/port[host = $host][threat = 'High']/node()">High</xsl:when>
      <xsl:when test="openvas:report()/ports/port[host = $host][threat = 'Medium']/node()">Medium</xsl:when>
      <xsl:when test="openvas:report()/ports/port[host = $host][threat = 'Low']/node()">Low</xsl:when>
      <xsl:when test="openvas:report()/ports/port[host = $host][threat = 'Log']/node()">Log</xsl:when>
      <xsl:when test="openvas:report()/ports/port[host = $host][threat = 'False Positive']/node()">False Positive</xsl:when>
      <xsl:otherwise>None</xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- Row in table with count of issues for a single host. -->
  <xsl:template name="results-overview-table-single-host-row">
    <xsl:variable name="host" select="ip"/>
    <xsl:variable name="hostname" select="detail[name/text() = 'hostname']/value"/>
    <xsl:choose>
      <xsl:when test="$hostname">
        <xsl:call-template name="latex-hyperref">
          <xsl:with-param name="target" select="concat('host:',$host)"/>
          <xsl:with-param name="text" select="concat($host, ' (', $hostname, ')')"/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="latex-hyperref">
          <xsl:with-param name="target" select="concat('host:',$host)"/>
          <xsl:with-param name="text" select="$host"/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
    <xsl:text>&amp;Severity: </xsl:text>
    <xsl:call-template name="highest-severity-for-host">
      <xsl:with-param name="host" select="$host"/>
    </xsl:call-template>
    <xsl:text>&amp;</xsl:text>
    <xsl:value-of select="count(../results/result[host=$host][threat/text()='High'])"/>
    <xsl:text>&amp;</xsl:text>
    <xsl:value-of select="count(../results/result[host=$host][threat/text()='Medium'])"/>
    <xsl:text>&amp;</xsl:text>
    <xsl:value-of select="count(../results/result[host=$host][threat/text()='Low'])"/>
    <xsl:text>&amp;</xsl:text>
    <xsl:value-of select="count(../results/result[host=$host][threat/text()='Log'])"/>
    <xsl:text>&amp;</xsl:text>
    <xsl:value-of select="count(../results/result[host=$host][threat/text()='False Positive'])"/>
    <xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
  </xsl:template>

  <!-- The Results Overview section. -->
  <xsl:template name="results-overview">
    <xsl:call-template name="latex-section">
      <xsl:with-param name="section_string">Result Overview</xsl:with-param>
    </xsl:call-template>
    <xsl:call-template name="newline"/>

    <xsl:text>\begin{longtable}{|l|l|l|l|l|l|l|}</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:call-template name="longtable-continue-block">
      <xsl:with-param name="number-of-columns">6</xsl:with-param>
      <xsl:with-param name="header-color">openvas_report</xsl:with-param>
      <xsl:with-param name="header-text">Host&amp;Most Severe Result(s)&amp;High&amp;Medium&amp;Low&amp;Log&amp;False Positives</xsl:with-param>
    </xsl:call-template>
    <xsl:for-each select="host"><xsl:call-template name="results-overview-table-single-host-row"/></xsl:for-each>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>Total: </xsl:text>
    <xsl:value-of select="count(openvas:report()/host_start)"/>&amp;&amp;<xsl:value-of select="count(openvas:report()/results/result[threat = 'High'])"/>&amp;<xsl:value-of select="count(openvas:report()/results/result[threat = 'Medium'])"/>&amp;<xsl:value-of select="count(openvas:report()/results/result[threat = 'Low'])"/>&amp;<xsl:value-of select="count(openvas:report()/results/result[threat = 'Log'])"/>&amp;<xsl:value-of select="count(openvas:report()/results/result[threat = 'False Positive'])"/><xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\end{longtable}</xsl:text><xsl:call-template name="newline"/>

    <xsl:choose>
      <xsl:when test="openvas:report()/@type = 'prognostic'">
      </xsl:when>
      <xsl:otherwise>
        <xsl:choose>
          <xsl:when test="openvas:report()/filters/autofp/text()='1'">
            <xsl:text>Vendor security updates are trusted, using full CVE matching.</xsl:text>
          </xsl:when>
          <xsl:when test="openvas:report()/filters/autofp/text()='2'">
            <xsl:text>Vendor security updates are trusted, using partial CVE matching.</xsl:text>
          </xsl:when>
          <xsl:otherwise>
            <xsl:text>Vendor security updates are not trusted.</xsl:text>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:call-template name="latex-newline"/>
        <xsl:choose>
          <xsl:when test="openvas:report()/filters/apply_overrides/text()='1'">
            <xsl:text>Overrides are on.  When a result has an override, this report uses the threat of the override.</xsl:text>
            <xsl:call-template name="latex-newline"/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:text>Overrides are off.  Even when a result has an override, this report uses the actual threat of the result.</xsl:text>
            <xsl:call-template name="latex-newline"/>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:choose>
          <xsl:when test="openvas:report()/filters/notes = 0">
            <xsl:text>Notes are excluded from the report.</xsl:text>
            <xsl:call-template name="latex-newline"/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:text>Notes are included in the report.</xsl:text>
            <xsl:call-template name="latex-newline"/>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:otherwise>
    </xsl:choose>
    <xsl:text>This report might not show details of all issues that were found.</xsl:text><xsl:call-template name="latex-newline"/>
    <xsl:if test="openvas:report()/filters/result_hosts_only = 1">
      <xsl:text>It only lists hosts that produced issues.</xsl:text><xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="string-length(openvas:report()/filters/phrase) &gt; 0">
      <xsl:text>It shows issues that contain the search phrase "</xsl:text><xsl:value-of select="openvas:report()/filters/phrase"/><xsl:text>".</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="contains(openvas:report()/filters/text(), 'h') = false">
      <xsl:text>Issues with the threat level "High" are not shown.</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="contains(openvas:report()/filters/text(), 'm') = false">
      <xsl:text>Issues with the threat level "Medium" are not shown.</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="contains(openvas:report()/filters/text(), 'l') = false">
      <xsl:text>Issues with the threat level "Low" are not shown.</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="contains(openvas:report()/filters/text(), 'g') = false">
      <xsl:text>Issues with the threat level "Log" are not shown.</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="contains(openvas:report()/filters/text(), 'd') = false">
      <xsl:text>Issues with the threat level "Debug" are not shown.</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:if test="contains(openvas:report()/filters/text(), 'f') = false">
      <xsl:text>Issues with the threat level "False Positive" are not shown.</xsl:text>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:call-template name="latex-newline"/>

    <xsl:variable name="last" select="openvas:report()/results/@start + count(openvas:report()/results/result) - 1"/>
    <xsl:choose>
      <xsl:when test="$last = 0">
        <xsl:text>This report contains 0 results.</xsl:text>
      </xsl:when>
      <xsl:when test="$last = openvas:report()/results/@start">
        <xsl:text>This report contains result </xsl:text>
        <xsl:value-of select="$last"/>
        <xsl:text> of the </xsl:text>
        <xsl:value-of select="openvas:report()/result_count/filtered"/>
        <xsl:text> results selected by the</xsl:text>
        <xsl:text> filtering above.</xsl:text>
      </xsl:when>
      <xsl:when test="$last = openvas:report()/result_count/filtered">
        <xsl:text>This report contains all </xsl:text>
        <xsl:value-of select="openvas:report()/result_count/filtered"/>
        <xsl:text> results selected by the</xsl:text>
        <xsl:text> filtering described above.</xsl:text>
      </xsl:when>
      <xsl:otherwise>
        <xsl:text>This report contains results </xsl:text>
        <xsl:value-of select="openvas:report()/results/@start"/>
        <xsl:text> to </xsl:text>
        <xsl:value-of select="$last"/>
        <xsl:text> of the </xsl:text>
        <xsl:value-of select="openvas:report()/result_count/filtered"/>
        <xsl:text> results selected by the</xsl:text>
        <xsl:text> filtering described above.</xsl:text>
      </xsl:otherwise>
    </xsl:choose>
    <xsl:choose>
      <xsl:when test="openvas:report()/@type = 'prognostic'">
      </xsl:when>
      <xsl:when test="openvas:report()/delta">
      </xsl:when>
      <xsl:otherwise>
        <xsl:text>  Before filtering there were </xsl:text>
        <xsl:value-of select="openvas:report()/result_count/text()"/>
        <xsl:text> results.</xsl:text>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- In Host-wise overview row in table. -->
  <xsl:template name="single-host-overview-table-row">
    <xsl:param name="threat"/>
    <xsl:param name="host"/>
    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:variable name="port_service" select="text()"/>
        <xsl:if test="openvas:report()/results/result[host=$host][threat/text()=$threat][port=$port_service]">
          <xsl:call-template name="latex-hyperref">
            <xsl:with-param name="target" select="concat('port:', $host, ' ', $port_service, ' ', $threat)"/>
            <xsl:with-param name="text" select="$port_service"/>
          </xsl:call-template>
          <xsl:text>&amp;</xsl:text><xsl:value-of select="$threat"/><xsl:call-template name="latex-newline"/>
          <xsl:call-template name="latex-hline"/>
        </xsl:if>
    </xsl:for-each>
  </xsl:template>

  <!-- Overview table for subsect. of details of findings for a single host. -->
  <xsl:template name="results-per-host-single-host-port-findings">
    <xsl:variable name="host" select="ip"/>
    <xsl:text>\begin{longtable}{|l|l|}</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:call-template name="longtable-continue-block">
      <xsl:with-param name="number-of-columns">2</xsl:with-param>
      <xsl:with-param name="header-color">openvas_report</xsl:with-param>
      <xsl:with-param name="header-text">Service (Port)&amp;Threat Level</xsl:with-param>
    </xsl:call-template>
    <xsl:call-template name="single-host-overview-table-row">
      <xsl:with-param name="threat">High</xsl:with-param>
      <xsl:with-param name="host" select="$host"/>
    </xsl:call-template>
    <xsl:call-template name="single-host-overview-table-row">
      <xsl:with-param name="threat">Medium</xsl:with-param>
      <xsl:with-param name="host" select="$host"/>
    </xsl:call-template>
    <xsl:call-template name="single-host-overview-table-row">
      <xsl:with-param name="threat">Low</xsl:with-param>
      <xsl:with-param name="host" select="$host"/>
    </xsl:call-template>
    <xsl:call-template name="single-host-overview-table-row">
      <xsl:with-param name="threat">Log</xsl:with-param>
      <xsl:with-param name="host" select="$host"/>
    </xsl:call-template>
    <xsl:call-template name="single-host-overview-table-row">
      <xsl:with-param name="threat">False Positive</xsl:with-param>
      <xsl:with-param name="host" select="$host"/>
    </xsl:call-template>
    <xsl:text>\end{longtable}</xsl:text><xsl:call-template name="newline"/>
  </xsl:template>

  <!-- Table of Closed CVEs for a single host. -->
  <xsl:template name="results-per-host-single-host-closed-cves">
    <xsl:variable name="cves" select="str:split(detail[name = 'Closed CVEs']/value, ',')"/>
    <xsl:choose>
      <xsl:when test="openvas:report()/@type = 'delta'">
      </xsl:when>
      <xsl:when test="openvas:report()/filters/show_closed_cves = 1">
        <xsl:text>\begin{longtable}{|l|l|}</xsl:text><xsl:call-template name="newline"/>
        <xsl:call-template name="latex-hline"/>
        <xsl:call-template name="longtable-continue-block">
          <xsl:with-param name="number-of-columns">2</xsl:with-param>
          <xsl:with-param name="header-color">openvas_report</xsl:with-param>
          <xsl:with-param name="header-text">Closed CVE&amp;NVT</xsl:with-param>
        </xsl:call-template>
        <xsl:variable name="host" select="."/>
        <xsl:for-each select="$cves">
          <xsl:value-of select="."/>
          <xsl:text>&amp;</xsl:text>
          <xsl:variable name="cve" select="normalize-space(.)"/>
          <xsl:variable name="closed_cve"
                        select="$host/detail[name = 'Closed CVE' and contains(value, $cve)]"/>
          <xsl:value-of select="$closed_cve/source/description"/>
          <xsl:call-template name="latex-newline"/>
          <xsl:call-template name="latex-hline"/>
        </xsl:for-each>
        <xsl:text>\end{longtable}</xsl:text><xsl:call-template name="newline"/>
      </xsl:when>
    </xsl:choose>
  </xsl:template>

  <!-- Colors for threats. -->
  <xsl:template name="threat-to-color">
    <xsl:param name="threat"/>
    <xsl:choose>
      <xsl:when test="threat='High'">openvas_hole</xsl:when>
      <xsl:when test="threat='Medium'">openvas_warning</xsl:when>
      <xsl:when test="threat='Low'">openvas_note</xsl:when>
      <xsl:when test="threat='Log'">openvas_log</xsl:when>
      <xsl:when test="threat='False Positive'">openvas_log</xsl:when>
    </xsl:choose>
  </xsl:template>

  <!-- Text of threat, Log to empty string. -->
  <xsl:template name="threat-to-severity">
    <xsl:param name="threat"/>
    <xsl:choose>
      <xsl:when test="threat='Low'">Low</xsl:when>
      <xsl:when test="threat='Medium'">Medium</xsl:when>
      <xsl:when test="threat='High'">High</xsl:when>
      <xsl:when test="threat='Log'"></xsl:when>
      <!-- TODO False Positive -->
    </xsl:choose>
  </xsl:template>

  <!-- References box. -->
  <xsl:template name="references">
    <xsl:variable name="cve_ref">
      <xsl:if test="nvt/cve != '' and nvt/cve != 'NOCVE'">
        <xsl:value-of select="nvt/cve/text()"/>
      </xsl:if>
    </xsl:variable>
    <xsl:variable name="bid_ref">
      <xsl:if test="nvt/bid != '' and nvt/bid != 'NOBID'">
        <xsl:value-of select="nvt/bid/text()"/>
      </xsl:if>
    </xsl:variable>
    <xsl:variable name="xref_ref">
      <xsl:if test="nvt/xref != '' and nvt/xref != 'NOXREF'">
        <xsl:value-of select="nvt/xref/text()"/>
      </xsl:if>
    </xsl:variable>

    <xsl:if test="$cve_ref != '' or $bid_ref != '' or $xref_ref != ''">
      <xsl:call-template name="latex-newline"/>
      \hline
      <xsl:call-template name="latex-newline"/>
      <xsl:text>\textbf{References}</xsl:text>
      <xsl:call-template name="latex-newline"/>
      <xsl:if test="$cve_ref != ''">
        <xsl:call-template name="text-to-escaped-row">
          <xsl:with-param name="string" select="concat('CVE: ', $cve_ref)"/>
        </xsl:call-template>
      </xsl:if>
      <xsl:if test="$bid_ref != ''">
        <xsl:call-template name="text-to-escaped-row">
          <xsl:with-param name="string" select="concat('BID:', $bid_ref)"/>
        </xsl:call-template>
      </xsl:if>
      <xsl:if test="$xref_ref != ''">
        <xsl:call-template name="text-to-escaped-row">
          <xsl:with-param name="string" select="'Other:'"/>
        </xsl:call-template>
        <xsl:for-each select="str:split($xref_ref, ',')">
          <xsl:call-template name="text-to-escaped-row">
            <xsl:with-param name="string" select="concat('  ', .)"/>
          </xsl:call-template>
        </xsl:for-each>
      </xsl:if>
    </xsl:if>
  </xsl:template>

  <!-- Text of a note. -->
  <xsl:template name="notes">
    <xsl:param name="delta">0</xsl:param>
    <xsl:if test="count(notes/note) &gt; 0">
      <xsl:call-template name="latex-hline"/>
      <xsl:call-template name="latex-newline"/>
    </xsl:if>
    <xsl:for-each select="notes/note">
      <xsl:call-template name="latex-newline"/>
      <xsl:text>\rowcolor{openvas_user_note}{\textbf{Note}</xsl:text>
      <xsl:if test="$delta and $delta &gt; 0"> (Result <xsl:value-of select="$delta"/>)</xsl:if>
      <xsl:text>}</xsl:text>\\<xsl:call-template name="latex-newline"/>
      <xsl:call-template name="text-to-escaped-row-color">
        <xsl:with-param name="color" select="'openvas_user_note'"/>
        <xsl:with-param name="string" select="text"/>
      </xsl:call-template>
      <xsl:text>\rowcolor{openvas_user_note}{}</xsl:text><xsl:call-template name="latex-newline"/>
      <xsl:choose>
        <xsl:when test="active='0'">
        </xsl:when>
        <xsl:when test="active='1' and string-length (end_time) &gt; 0">
          <xsl:text>\rowcolor{openvas_user_note}{Active until: </xsl:text>
          <xsl:call-template name="format-date">
            <xsl:with-param name="date" select="end_time"/>
          </xsl:call-template>
          <xsl:text>}</xsl:text><xsl:call-template name="latex-newline"/>
        </xsl:when>
        <xsl:otherwise>
        </xsl:otherwise>
      </xsl:choose>
      <xsl:text>\rowcolor{openvas_user_note}{Last modified: </xsl:text>
      <xsl:call-template name="format-date">
        <xsl:with-param name="date" select="modification_time"/>
      </xsl:call-template>
      <xsl:text>}</xsl:text><xsl:call-template name="latex-newline"/>
    </xsl:for-each>
  </xsl:template>

  <!-- Text of an override. -->
  <xsl:template name="overrides">
    <xsl:param name="delta">0</xsl:param>
    <xsl:if test="openvas:report()/filters/apply_overrides/text()='1'">
      <xsl:if test="count(overrides/override) &gt; 0">
        <xsl:call-template name="latex-hline"/>
        <xsl:call-template name="latex-newline"/>
      </xsl:if>
      <xsl:for-each select="overrides/override">
        <xsl:call-template name="latex-newline"/>
        <xsl:text>\rowcolor{openvas_user_override}{\textbf{Override from </xsl:text>
        <xsl:choose>
          <xsl:when test="string-length(threat) = 0">
            <xsl:text>Any</xsl:text>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select="threat"/>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:text> to </xsl:text>
        <xsl:value-of select="new_threat"/><xsl:text>}</xsl:text>
        <xsl:if test="$delta and $delta &gt; 0"> (Result <xsl:value-of select="$delta"/>)</xsl:if>
        <xsl:text>}</xsl:text>\\<xsl:call-template name="latex-newline"/>
        <xsl:call-template name="text-to-escaped-row-color">
          <xsl:with-param name="color" select="'openvas_user_override'"/>
          <xsl:with-param name="string" select="text"/>
        </xsl:call-template>
        <xsl:text>\rowcolor{openvas_user_override}{}</xsl:text><xsl:call-template name="latex-newline"/>
        <xsl:choose>
          <xsl:when test="active='0'">
          </xsl:when>
          <xsl:when test="active='1' and string-length (end_time) &gt; 0">
            <xsl:text>\rowcolor{openvas_user_override}{Active until: </xsl:text>
            <xsl:call-template name="format-date">
              <xsl:with-param name="date" select="end_time"/>
            </xsl:call-template>
            <xsl:text>}</xsl:text><xsl:call-template name="latex-newline"/>
          </xsl:when>
          <xsl:otherwise>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:text>\rowcolor{openvas_user_override}{Last modified: </xsl:text>
        <xsl:call-template name="format-date">
          <xsl:with-param name="date" select="modification_time"/>
        </xsl:call-template>
        <xsl:text>}</xsl:text><xsl:call-template name="latex-newline"/>
      </xsl:for-each>
    </xsl:if>
  </xsl:template>

<!-- SUBSECTION: Results for a single host. -->

  <!-- Overview table for a single host -->
  <xsl:template name="result-details-host-port-threat">
    <xsl:param name="host"/>
    <xsl:param name="port_service"/>
    <xsl:param name="threat"/>
    <xsl:if test="openvas:report()/results/result[host=$host][threat/text()=$threat][port=$port_service]">
      <xsl:call-template name="latex-subsubsection"><xsl:with-param name="subsubsection_string" select="concat ($threat, ' ', $port_service)"/></xsl:call-template>
      <xsl:call-template name="latex-label"><xsl:with-param name="label_string" select="concat('port:', $host, ' ', $port_service, ' ', $threat)"/></xsl:call-template>
      <xsl:call-template name="newline"/>
      <xsl:for-each select="openvas:report()/results/result[host=$host][threat/text()=$threat][port=$port_service]">
        <xsl:text>\begin{longtable}{|p{\textwidth * 1}|}</xsl:text><xsl:call-template name="newline"/>
        <xsl:call-template name="latex-hline"/>
        <xsl:text>\rowcolor{</xsl:text>
        <xsl:call-template name="threat-to-color">
          <xsl:with-param name="threat" select="$threat" />
        </xsl:call-template>
        <xsl:text>}{\color{white}{</xsl:text>
        <xsl:if test="delta/text()">
          <xsl:text>\vspace{3pt}</xsl:text>
          <xsl:text>\hspace{3pt}</xsl:text>
          <xsl:choose>
            <xsl:when test="delta/text() = 'changed'">\begin{LARGE}\sim\end{LARGE}</xsl:when>
            <xsl:when test="delta/text() = 'gone'">\begin{LARGE}&#8722;\end{LARGE}</xsl:when>
            <xsl:when test="delta/text() = 'new'">\begin{LARGE}+\end{LARGE}</xsl:when>
            <xsl:when test="delta/text() = 'same'">\begin{LARGE}=\end{LARGE}</xsl:when>
          </xsl:choose>
          <xsl:text>\hspace{3pt}</xsl:text>
        </xsl:if>
        <xsl:value-of select="$threat"/>
        <xsl:choose>
          <xsl:when test="original_threat">
            <xsl:choose>
              <xsl:when test="threat = original_threat">
                <xsl:if test="string-length(nvt/cvss_base) &gt; 0">
                  <xsl:text> (CVSS: </xsl:text>
                  <xsl:value-of select="nvt/cvss_base"/>
                  <xsl:text>) </xsl:text>
                </xsl:if>
              </xsl:when>
              <xsl:otherwise>
                <xsl:text> (Overridden from </xsl:text>
                <xsl:value-of select="original_threat"/>
                <xsl:text>) </xsl:text>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:when>
          <xsl:otherwise>
            <xsl:if test="string-length(nvt/cvss_base) &gt; 0">
              <xsl:text> (CVSS: </xsl:text>
              <xsl:value-of select="nvt/cvss_base"/>
              <xsl:text>) </xsl:text>
            </xsl:if>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:text>}}</xsl:text>
        <xsl:call-template name="latex-newline"/>
        <xsl:text>\rowcolor{</xsl:text>
        <xsl:call-template name="threat-to-color">
          <xsl:with-param name="threat" select="$threat"/>
        </xsl:call-template>
        <xsl:text>}{\color{white}{NVT: </xsl:text>
        <xsl:variable name="name_escaped"><xsl:call-template name="escape_text"><xsl:with-param name="string" select="nvt/name"/></xsl:call-template></xsl:variable>
        <xsl:value-of select="$name_escaped"/>
        <xsl:text>}}</xsl:text>
        <xsl:call-template name="latex-newline"/>
        <xsl:call-template name="latex-hline"/>
        <xsl:text>\endfirsthead</xsl:text><xsl:call-template name="newline"/>
        <xsl:text>\hfill\ldots continued from previous page \ldots </xsl:text><xsl:call-template name="latex-newline"/>
        <xsl:call-template name="latex-hline"/>
        <xsl:text>\endhead</xsl:text><xsl:call-template name="newline"/>
        <xsl:call-template name="latex-hline"/>
        <xsl:text>\ldots continues on next page \ldots </xsl:text><xsl:call-template name="latex-newline"/>
        <xsl:text>\endfoot</xsl:text><xsl:call-template name="newline"/>
        <xsl:call-template name="latex-hline"/>
        <xsl:text>\endlastfoot</xsl:text><xsl:call-template name="newline"/>

        <xsl:if test="count (detection)">
          <xsl:call-template name="latex-newline"/>
          <xsl:text>\textbf{Product detection result}</xsl:text>
          <xsl:call-template name="latex-newline"/>
          <xsl:call-template name="text-to-escaped-row">
            <xsl:with-param name="string" select="detection/result/details/detail[name = 'product']/value/text()"/>
          </xsl:call-template>
          <xsl:call-template name="text-to-escaped-row">
            <xsl:with-param name="string" select="concat('Detected by ', detection/result/details/detail[name = 'source_name']/value/text(), ' (OID: ', detection/result/details/detail[name = 'source_oid']/value/text(), ')')"/>
          </xsl:call-template>
          <xsl:call-template name="latex-newline"/>
          <xsl:call-template name="latex-hline"/>
        </xsl:if>

        <xsl:choose>
          <xsl:when test="delta/text() = 'changed'">
            <xsl:call-template name="latex-newline"/>
            <xsl:text>\textbf{Result 1}</xsl:text>
            <xsl:call-template name="latex-newline"/>
          </xsl:when>
        </xsl:choose>
        <xsl:call-template name="latex-newline"/>
        <xsl:call-template name="text-to-escaped-row">
          <xsl:with-param name="string" select="description"/>
        </xsl:call-template>
        <xsl:text>\rowcolor{white}{\verb==}</xsl:text><xsl:call-template name="latex-newline"/>
        <xsl:text>\rowcolor{white}{\verb==}</xsl:text><xsl:call-template name="latex-newline"/>
        <xsl:call-template name="latex-newline"/>
        <xsl:text>OID of test routine: </xsl:text><xsl:value-of select="nvt/@oid"/>
        <xsl:call-template name="latex-newline"/>
        <xsl:if test="delta">
          <xsl:choose>
            <xsl:when test="delta/text() = 'changed'">

              <xsl:call-template name="latex-hline"/>
              <xsl:call-template name="latex-newline"/>
              <xsl:text>\textbf{Result 2}</xsl:text>
              <xsl:call-template name="latex-newline"/>
              <xsl:call-template name="latex-newline"/>
              <xsl:call-template name="text-to-escaped-row">
                <xsl:with-param name="string" select="delta/result/description"/>
              </xsl:call-template>
              <xsl:text>\rowcolor{white}{\verb==}</xsl:text><xsl:call-template name="latex-newline"/>
              <xsl:text>\rowcolor{white}{\verb==}</xsl:text><xsl:call-template name="latex-newline"/>
              <xsl:call-template name="latex-newline"/>
              <xsl:text>OID of test routine: </xsl:text><xsl:value-of select="delta/result/nvt/@oid"/>
              <xsl:call-template name="latex-newline"/>

              <xsl:call-template name="latex-hline"/>
              <xsl:call-template name="latex-newline"/>
              <xsl:text>\textbf{Different Lines}</xsl:text>
              <xsl:call-template name="latex-newline"/>
              <xsl:call-template name="latex-newline"/>
              <xsl:call-template name="text-to-escaped-diff-row">
                <xsl:with-param name="string" select="delta/diff"/>
              </xsl:call-template>
              <xsl:call-template name="latex-newline"/>
            </xsl:when>
          </xsl:choose>
        </xsl:if>
        <xsl:call-template name="references"/>
        <xsl:variable name="delta">
          <xsl:choose>
            <xsl:when test="delta">1</xsl:when>
            <xsl:otherwise>0</xsl:otherwise>
          </xsl:choose>
        </xsl:variable>
        <xsl:call-template name="notes">
          <xsl:with-param name="delta" select="$delta"/>
        </xsl:call-template>
        <xsl:for-each select="delta">
          <xsl:call-template name="notes">
            <xsl:with-param name="delta" select="2"/>
          </xsl:call-template>
        </xsl:for-each>
        <xsl:call-template name="overrides">
          <xsl:with-param name="delta" select="$delta"/>
        </xsl:call-template>
        <xsl:for-each select="delta">
          <xsl:call-template name="overrides">
            <xsl:with-param name="delta" select="2"/>
          </xsl:call-template>
        </xsl:for-each>
        <xsl:text>\end{longtable}</xsl:text>
        <xsl:call-template name="newline"/>
        <xsl:call-template name="newline"/>
      </xsl:for-each>

      <xsl:text>\begin{footnotesize}</xsl:text>
      <xsl:call-template name="latex-hyperref">
        <xsl:with-param name="target" select="concat('host:', $host)"/>
        <xsl:with-param name="text" select="concat('[ return to ', $host, ' ]')"/>
      </xsl:call-template>

      <xsl:text>\end{footnotesize}</xsl:text><xsl:call-template name="newline"/>
    </xsl:if>
  </xsl:template>

  <!-- A prognostic result. -->
  <xsl:template name="prognostic-result">
    <xsl:text>\begin{longtable}{|p{\textwidth * 1}|}</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\rowcolor{</xsl:text>
    <xsl:call-template name="threat-to-color">
      <xsl:with-param name="threat" select="threat" />
    </xsl:call-template>
    <xsl:text>}{\color{white}{</xsl:text>
    <xsl:value-of select="threat"/>
    <xsl:if test="string-length(cve/cvss_base) &gt; 0">
      <xsl:text> (CVSS: </xsl:text>
      <xsl:value-of select="cve/cvss_base"/>
      <xsl:text>) </xsl:text>
    </xsl:if>
    <xsl:text>}}</xsl:text>
    <xsl:call-template name="latex-newline"/>
    <xsl:text>\rowcolor{</xsl:text>
    <xsl:call-template name="threat-to-color">
      <xsl:with-param name="threat" select="threat"/>
    </xsl:call-template>
    <xsl:text>}{\color{white}{</xsl:text>
    <xsl:value-of select="cve/@id"/>
    <xsl:text>}}</xsl:text>
    <xsl:call-template name="latex-newline"/>
    <xsl:text>\rowcolor{</xsl:text>
    <xsl:call-template name="threat-to-color">
      <xsl:with-param name="threat" select="threat"/>
    </xsl:call-template>
    <xsl:text>}{\color{white}{</xsl:text>
    <xsl:call-template name="escape_text">
      <xsl:with-param name="string" select="cve/cpe/@id"/>
    </xsl:call-template>
    <xsl:text>}}</xsl:text>
    <xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\endfirsthead</xsl:text><xsl:call-template name="newline"/>
    <xsl:text>\hfill\ldots continued from previous page \ldots </xsl:text><xsl:call-template name="latex-newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\endhead</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\ldots continues on next page \ldots </xsl:text><xsl:call-template name="latex-newline"/>
    <xsl:text>\endfoot</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="latex-hline"/>
    <xsl:text>\endlastfoot</xsl:text><xsl:call-template name="newline"/>

    <xsl:call-template name="text-to-escaped-row">
      <xsl:with-param name="string" select="description"/>
    </xsl:call-template>
    <xsl:call-template name="latex-newline"/>

    <xsl:text>\end{longtable}</xsl:text>
    <xsl:call-template name="newline"/>
    <xsl:call-template name="newline"/>
  </xsl:template>

  <!-- Findings for a single prognostic host -->
  <xsl:template name="results-per-host-prognostic">
    <xsl:param name="host"/>

    <xsl:for-each select="openvas:report()/results/result[host=$host][threat='High']">
      <xsl:call-template name="prognostic-result"/>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/results/result[host=$host][threat='Medium']">
      <xsl:call-template name="prognostic-result"/>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/results/result[host=$host][threat='Low']">
      <xsl:call-template name="prognostic-result"/>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/results/result[host=$host][threat='Log']">
      <xsl:call-template name="prognostic-result"/>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/results/result[host=$host][threat='Debug']">
      <xsl:call-template name="prognostic-result"/>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/results/result[host=$host][threat='False Positive']">
      <xsl:call-template name="prognostic-result"/>
    </xsl:for-each>

    <xsl:text>\begin{footnotesize}</xsl:text>
    <xsl:call-template name="latex-hyperref">
      <xsl:with-param name="target" select="concat('host:', $host)"/>
      <xsl:with-param name="text" select="concat('[ return to ', $host, ' ]')"/>
    </xsl:call-template>

    <xsl:text>\end{footnotesize}</xsl:text><xsl:call-template name="newline"/>
  </xsl:template>

  <!-- Findings for a single host -->
  <xsl:template name="results-per-host-single-host-findings">
    <xsl:param name="host"/>

    <!-- TODO Solve other sorting possibilities. -->
    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:call-template name="result-details-host-port-threat">
        <xsl:with-param name="threat">High</xsl:with-param>
        <xsl:with-param name="host" select="$host"/>
        <xsl:with-param name="port_service" select="text()"/>
      </xsl:call-template>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:call-template name="result-details-host-port-threat">
        <xsl:with-param name="threat">Medium</xsl:with-param>
        <xsl:with-param name="host" select="$host"/>
        <xsl:with-param name="port_service" select="text()"/>
      </xsl:call-template>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:call-template name="result-details-host-port-threat">
        <xsl:with-param name="threat">Low</xsl:with-param>
        <xsl:with-param name="host" select="$host"/>
        <xsl:with-param name="port_service" select="text()"/>
      </xsl:call-template>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:call-template name="result-details-host-port-threat">
        <xsl:with-param name="threat">Log</xsl:with-param>
        <xsl:with-param name="host" select="$host"/>
        <xsl:with-param name="port_service" select="text()"/>
      </xsl:call-template>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:call-template name="result-details-host-port-threat">
        <xsl:with-param name="threat">Debug</xsl:with-param>
        <xsl:with-param name="host" select="$host"/>
        <xsl:with-param name="port_service" select="text()"/>
      </xsl:call-template>
    </xsl:for-each>

    <xsl:for-each select="openvas:report()/ports/port[host=$host]">
      <xsl:call-template name="result-details-host-port-threat">
        <xsl:with-param name="threat">False Positive</xsl:with-param>
        <xsl:with-param name="host" select="$host"/>
        <xsl:with-param name="port_service" select="text()"/>
      </xsl:call-template>
    </xsl:for-each>
  </xsl:template>

  <!-- Subsection for a single host, with all details. -->
  <xsl:template name="results-per-host-single-host">
    <xsl:variable name="host" select="ip"/>
    <xsl:call-template name="latex-subsection">
      <xsl:with-param name="subsection_string" select="$host"/>
    </xsl:call-template>
    <xsl:call-template name="latex-label">
      <xsl:with-param name="label_string" select="concat('host:', $host)"/>
    </xsl:call-template>
    <xsl:call-template name="newline"/>

    <xsl:choose>
      <xsl:when test="openvas:report()/@type = 'prognostic'">
      </xsl:when>
      <xsl:otherwise>
        <xsl:text>\begin{tabular}{ll}</xsl:text><xsl:call-template name="newline"/>
        <xsl:text>Host scan start&amp;</xsl:text>
        <xsl:call-template name="format-date">
          <xsl:with-param name="date" select="start"/>
        </xsl:call-template>
        <xsl:call-template name="latex-newline"/>
        <xsl:text>Host scan end&amp;</xsl:text>
        <xsl:call-template name="format-date">
          <xsl:with-param name="date" select="end"/>
        </xsl:call-template>
        <xsl:call-template name="latex-newline"/>
        <xsl:text>\end{tabular}</xsl:text><xsl:call-template name="newline"/>
        <xsl:call-template name="newline"/>
        <xsl:call-template name="results-per-host-single-host-port-findings"/>
        <xsl:call-template name="newline"/>
        <xsl:call-template name="results-per-host-single-host-closed-cves"/>
        <xsl:call-template name="newline"/>
      </xsl:otherwise>
    </xsl:choose>

    <xsl:text>%\subsection*{Security Issues and Fixes -- </xsl:text><xsl:value-of select="$host"/><xsl:text>}</xsl:text>
    <xsl:call-template name="newline"/>
    <xsl:call-template name="newline"/>
    <xsl:choose>
      <xsl:when test="openvas:report()/@type = 'prognostic'">
        <xsl:call-template name="results-per-host-prognostic">
          <xsl:with-param name="host" select="$host"/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="results-per-host-single-host-findings"><xsl:with-param name="host" select="$host"/></xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- Section with Results per Host. -->
  <xsl:template name="results-per-host">
    <xsl:text>\section{Results per Host}</xsl:text>
    <xsl:call-template name="newline"/><xsl:call-template name="newline"/>
    <xsl:for-each select="host">
      <xsl:call-template name="results-per-host-single-host"/>
    </xsl:for-each>
  </xsl:template>

  <!-- The actual report. -->
  <xsl:template name="real-report">
    <xsl:call-template name="header"/>
    <xsl:call-template name="newline"/>
    <xsl:text>\begin{document}</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="newline"/>
    <xsl:text>\maketitle</xsl:text><xsl:call-template name="newline"/>
    <xsl:call-template name="abstract"/>
    <xsl:call-template name="toc"/>
    <xsl:text>\newpage</xsl:text>
    <xsl:call-template name="newline"/>
    <xsl:call-template name="results-overview"/>
    <xsl:call-template name="results-per-host"/>
    <xsl:text>
\begin{center}
\medskip
\rule{\textwidth}{0.1pt}

This file was automatically generated.
\end{center}

\end{document}
</xsl:text>
  </xsl:template>

  <!-- The first report element. -->
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

  <!-- Match the root. -->
  <xsl:template match="/">
    <xsl:apply-templates/>
  </xsl:template>

</xsl:stylesheet>
