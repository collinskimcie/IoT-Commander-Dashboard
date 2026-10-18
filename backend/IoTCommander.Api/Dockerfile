FROM mcr.microsoft.com/dotnet/sdk:8.0 AS build
WORKDIR /src

COPY IoTCommander.Api.csproj ./
RUN dotnet restore IoTCommander.Api.csproj --force --no-cache

COPY . ./
RUN dotnet publish IoTCommander.Api.csproj -c Release -o /app/publish

FROM mcr.microsoft.com/dotnet/aspnet:8.0 AS runtime
WORKDIR /app
COPY --from=build /app/publish ./
ENV ASPNETCORE_URLS=http://+:8080
EXPOSE 8080
ENTRYPOINT ["dotnet", "IoTCommander.Api.dll"]
