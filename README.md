# mod_dav_calendar
This module extends a WebDAV server to add support for CalDAV, the calendaring server
protocol defined in RFC4791.

The CalDAV server extends a standard WebDAV server allowing iCal calender files to
be created and searched for by a calendar client.

This module can also combine multiple iCal resources together as created by a calendar
client at a single URL that can be subscribed to by another calendar client.

Requires Apache httpd v2.4.52 or higher.

# download

RPM Packages are available at
[COPR](https://copr.fedorainfracloud.org/coprs/minfrin/mod_dav_calendar/) for Fedora and OpenSUSE.

```
dnf copr enable minfrin/mod_dav_calendar
dnf install mod_dav_calendar
```

If you need to depend on mod_dav_access, enable it as follows:

```
dnf copr enable minfrin/mod_dav_access
dnf install mod_dav_access
```

Ubuntu packages are available through
[PPA](https://launchpad.net/~minfrin/+archive/ubuntu/apache2/).

# quick configuration

The configuration below gives the logged in user their own calandar
space.

    <IfModule !alias_module>
      LoadModule alias_module modules/mod_alias.so
    </IfModule>
    <IfModule !authz_core_module>
      LoadModule authz_core_module modules/mod_authz_core.so
    </IfModule>
    <IfModule !autoindex_module>
      LoadModule autoindex_module modules/mod_autoindex.so
    </IfModule>
    <IfModule !dav_module>
      LoadModule dav_module modules/mod_dav.so
    </IfModule>
    <IfModule !dav_fs_module>
      LoadModule dav_fs_module modules/mod_dav_fs.so
    </IfModule>
    <IfModule !dir_module>
      LoadModule dir_module modules/mod_dir.so
    </IfModule>
    <IfModule !setenvif_module>
     LoadModule setenvif_module modules/mod_setenvif.so
    </IfModule>

    Redirect /.well-known/caldav /calendar/

    <Location /calendar>
      Alias /var/www/dav/calendar/
      AliasPreservePath on

      Dav on
      DavAccess on
      DavCalendar on
      Options +Indexes

      DavAccessPriviledge all
      DavAccessPrincipalUrl /calendar/principals/%{escape:%{REMOTE_USER}}/
      DavCalendarHome /calendar/calendars/%{escape:%{REMOTE_USER}}/
      DavCalendarProvision /calendar/calendars/%{escape:%{REMOTE_USER}}/ %{REMOTE_USER}
      DavCalendarTimezone UTC

      IndexOptions FancyIndexing HTMLTable VersionSort XHTML
      DirectoryIndex disabled
      FileETag INode MTime Size

      # limit to logged in users
      AuthType basic

      SetEnvIf REQUEST_URI "^/calendar/calendars/([^/]+)" MATCH_USER=$1

      <RequireAll>
        require valid-user
        require expr %{env:MATCH_USER} == '' || %{unescape:%{env:MATCH_USER}} == %{REMOTE_USER}
      </RequireAll>

    </Location>

    <Location /calendar/principals>

      Alias /var/www/dav/calendar/principals
      AliasPreservePath off

      DavAccessPrincipal on

    </Location>

# configuration in more detail

## find the calendar

Some CalDAV clients use the "/.well-known/caldav/" path to find the calendar
URL space. Redirect this URL as follows.

    Redirect /.well-known/caldav /calendar/

## find the principal URL

Having been told the calendar URL space, CalDAV clients want to find the
principal of the logged in user, to see what that user is allowed to do, and
where their calendars are in the wider calendar URL space.

A PROPFIND is sent asking for the logged in user's principal URL. This URL represents the logged in user, and can be queried for information about that user.

Some WebDAV providers (like Subversion) provide their own principal URL functionality, while
others (like mod_dav_fs) do not. If needed, use mod_dav_access to define the location of
the principal URL.

    Alias /calendar /home/calendar
    <Directory /home/calendar>
      DavAccessPrincipalUrl /principal/%{escape:%{REMOTE_USER}}
    </Directory>

## find the calendar URL

The next step is perform a PROPFIND against the principal URL to ask for the location of the
user's calendars. These calendar URLs might be unique to a specific user, or might be shared
amongst many users.

    <Location /principal>
      DavCalendarHome /calendar/personal/%{escape:%{REMOTE_USER}}/
    </Location>

## access the calendar

Now we know where the calendar is, we can query the calendar. A calendar is a WebDAV directory
containing iCal files that can be searched using PROPFIND. The calendar client's job is to
create and modify the files, the server is a passive participant.

A calendar client might query the calendar URL for the user's calendars instead of the
pricipal URL, so we define the DavCalendarHome a second time.

Calendar clients expect the collections (the directories) belonging to a calendar to already
exist on the server. Use DavCalendarProvision to autocreate the required directories so that
calendar clients work automatically on first use.

    Alias /calendar /home/calendar
    <Directory /home/calendar>
      Dav on
      DavCalendar on
      DavCalendarHome /calendar/personal/%{escape:%{REMOTE_USER}}/
      DavCalendarProvision /calendar/personal/%{escape:%{REMOTE_USER}}/Home/
      DavCalendarTimezone UTC
    </Directory>

# configuration directives

The *DavCalendar* directive enables support for CALDAV compliant PROPFIND requests to a
given URL space. The directive is 'off' or 'on'.

The *DavCalendarTimezone* directive defines the default timezone to be used for
autoprovisioned calendars. Defaults to UTC.

The *DavCalendarMaxResourceSize* directive limits the size of individual iCal files on the
server. A calendar client will automatically split a calendar over multiple small files to
keep sizes within sensible limits. Defaults to 10MB.

The *DavCalendarHome* directive specifies the location of calendars in this URL space. The
parameter is an expression, which could resolve to an URL unique per user, or to a shared
URL common to many users.

The *DavCalendarProvision* directive defines the URL space of a collection that will be
autocreated on first access. The parameter is an expression that could resolve to a URL
unique to a user, or to a common shared URL. An optional second parameter allows the
displayname of the calendar to be specified.

The *DavCalendarAlias* directive makes a collection of iCal resources available combined
together at a single predictable URL. This can be used to allow a calendar to be subscribed
to at a well defined URL outside the calendar web space.

The *DavCalendarAliasMatch* directive makes a collection of iCal resources available
combined together at a predictable URL. In this directive a regular expression can be used
to match the calendar resources, and an expression can be used to define the final URL.

